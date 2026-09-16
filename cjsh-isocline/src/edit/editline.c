/*
  editline.c

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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include "attr.h"
#include "bbcode.h"
#include "fuzzy_match.h"  // IWYU pragma: keep
#include "keybindings.h"
#include "keycodes.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common.h"
#include "completions.h"
#include "editline_viewport.h"
#include "env.h"
#include "env_internal.h"
#include "highlight.h"
#include "history.h"
#include "isocline.h"
#include "prompt_line_replacement.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"
#include "undo.h"

//-------------------------------------------------------------
// The editor state
//-------------------------------------------------------------

// editor state
typedef struct editor_s {
    stringbuf_t* input;           // current user input
    stringbuf_t* extra;           // extra displayed info (for completion menu etc)
    stringbuf_t* status;          // transient status message below the prompt
    stringbuf_t* hint;            // hint displayed as part of the input
    stringbuf_t* hint_help;       // help for a hint.
    stringbuf_t* history_prefix;  // cached prefix before history navigation
    ssize_t pos;                  // current cursor position in the input
    ssize_t cur_rows;             // total logical rows for input and extra content
    ssize_t input_rows;           // logical prompt/input rows before status/help content
    ssize_t cur_row;              // logical row that has the cursor (0 based, relative to
                                  // the prompt)
    ssize_t view_first_row;       // first logical input row in the current viewport
    ssize_t view_rows;            // total rows physically rendered in the current viewport
    ssize_t view_input_rows;      // physically rendered prompt/input rows in the viewport
    ssize_t termw;
    ssize_t termh;
    bool modified;                      // has a modification happened? (used for history navigation
                                        // for example)
    bool disable_undo;                  // temporarily disable auto undo (for history search)
    bool refresh_suppressed;            // batch screen updates during high-volume input
    bool refresh_pending;               // remember to refresh when suppression lifts
    bool completion_auto_menu_visible;  // passive list owned by the main editor
    bool completion_menu_active;        // an interactive completion menu owns input
    ssize_t completion_auto_menu_header_rows;  // passive layout used for click activation
    ssize_t completion_auto_menu_item_rows;
    ssize_t completion_auto_menu_rows;
    bool history_prefix_active;         // whether prefix-prioritized history is active
    bool request_submit;                // request submission of current line
    bool force_linear_line_numbers;     // final render should drop relative numbering styling
    int64_t last_activity_ms;           // last non-resize input, including input handled by menus
    int64_t idle_deadline_ms;           // shared idle deadline for the editor and nested menus
    ssize_t history_idx;                // current index in the history
    bool last_arg_yank_active;          // whether yank-last-arg is cycling through history
    ssize_t last_arg_yank_history_idx;  // history index currently used for yank-last-arg
    ssize_t last_arg_yank_start;        // insertion start for yank-last-arg replacement
    ssize_t last_arg_yank_end;          // insertion end for yank-last-arg replacement
    editstate_t* undo;                  // undo buffer
    editstate_t* redo;                  // redo buffer
    const char* prompt_text;            // text of the prompt before the prompt marker
    char* prompt_prefix_text;     // cached multi-line prompt prefix (everything before last line)
    ssize_t prompt_prefix_lines;  // number of prefix lines emitted for prompt
    bool prompt_begins_with_newline;       // prompt started with a leading newline
    bool replace_prompt_line_with_number;  // should row 0 use line numbers instead of prompt text?
    bool
        force_prompt_text_visible;  // temporarily prevent prompt replacement (e.g., search prompts)
    const char* inline_right_text;  // inline right-aligned text on input line
    ssize_t inline_right_width;     // cached width of inline right text
    ssize_t line_number_column_width;  // cached total prefix width when line numbers are shown
    char* rendered_hint_snapshot;      // most recent hint text that was rendered on screen
    ssize_t last_screen_cursor_row;    // cached absolute cursor row from the last successful query
    ssize_t last_screen_cursor_col;    // cached absolute cursor col from the last successful query
    bool last_screen_cursor_known;     // whether absolute cursor position cache is valid
    ic_mouse_clicking_mode_t mouse_reporting_mode;  // per-session mouse capture strategy
    bool mouse_reporting_enabled;             // whether terminal mouse capture is currently on
    bool mouse_reporting_manual_enabled;      // user/default preference for this session
    bool mouse_reporting_auto_suspended;      // smart mode auto-disabled mouse capture
    bool mouse_terminal_selection_extra;      // rendered extra rows should select in the terminal
    bool mouse_terminal_selection_suspended;  // pause repainting until selection is acted on
    bool mouse_focus_reporting_enabled;       // focus-in/focus-out reporting (CSI I/O) enabled
    ssize_t mouse_capture_depth;              // nested mouse tracking enablement depth
    bool mouse_left_button_down;              // track click origins for terminals without motion
    ssize_t mouse_left_press_column;
    ssize_t mouse_left_press_row;
    alloc_t* mem;  // allocator
    // caches
    attrbuf_t* attrs;  // reuse attribute buffers
    attrbuf_t* attrs_extra;
} editor_t;

static ssize_t count_logical_lines(stringbuf_t* sbuf);

static bool has_continuation_prompt_marker(const ic_env_t* env) {
    return (env != NULL && env->cprompt_marker != NULL && env->cprompt_marker[0] != '\0');
}

static bool line_numbers_enabled(const ic_env_t* env) {
    if (env == NULL || !env->show_line_numbers) {
        return false;
    }
    if (has_continuation_prompt_marker(env) && !env->allow_line_numbers_with_continuation_prompt) {
        return false;
    }
    return true;
}

static bool prompt_line_should_use_line_numbers(const ic_env_t* env, const editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return false;
    }

    if (eb->force_prompt_text_visible) {
        return false;
    }

    ic_prompt_line_replacement_state_t predicate = {
        .replace_prompt_line_with_line_number = env->replace_prompt_line_with_line_number,
        .prompt_has_prefix_lines = (eb->prompt_prefix_lines > 0),
        .prompt_begins_with_newline = eb->prompt_begins_with_newline,
        .line_numbers_enabled = line_numbers_enabled(env),
        .input_has_content = (eb->input != NULL && sbuf_len(eb->input) > 0),
    };

    return ic_prompt_line_replacement_should_activate(&predicate);
}

static bool edit_completion_click_accept_enabled(const ic_env_t* env) {
    return (env != NULL && env->completion_click_accept_enabled);
}

static void edit_generate_completions(ic_env_t* env, editor_t* eb, bool autotab);
static void edit_refresh_completion_auto_menu(ic_env_t* env, editor_t* eb);
static void edit_history_search_with_current_line(ic_env_t* env, editor_t* eb);
static void edit_command_palette(ic_env_t* env, editor_t* eb);
static void edit_history_prev(ic_env_t* env, editor_t* eb);
static void edit_history_next(ic_env_t* env, editor_t* eb);
static void edit_yank_last_arg(ic_env_t* env, editor_t* eb);
static void edit_clear_history_preview(editor_t* eb);
static void edit_clear(ic_env_t* env, editor_t* eb);
static void edit_clear_with_prompt_prefix(ic_env_t* env, editor_t* eb, ssize_t prompt_prefix_lines);
static void edit_clear_screen(ic_env_t* env, editor_t* eb);
static void edit_undo_restore(ic_env_t* env, editor_t* eb);
static void edit_redo_restore(ic_env_t* env, editor_t* eb);
static void edit_show_help(ic_env_t* env, editor_t* eb);
static void edit_cursor_left(ic_env_t* env, editor_t* eb);
static void edit_cursor_right(ic_env_t* env, editor_t* eb);
static void edit_cursor_row_up(ic_env_t* env, editor_t* eb);
static void edit_history_prev_with_spell(ic_env_t* env, editor_t* eb);
static void edit_cursor_row_down(ic_env_t* env, editor_t* eb);
static void edit_cursor_line_start(ic_env_t* env, editor_t* eb);
static void edit_cursor_line_end(ic_env_t* env, editor_t* eb);
static void edit_cursor_ctrl_a(ic_env_t* env, editor_t* eb);
static void edit_cursor_ctrl_e(ic_env_t* env, editor_t* eb);
static void edit_cursor_prev_word(ic_env_t* env, editor_t* eb);
static void edit_cursor_next_word(ic_env_t* env, editor_t* eb);
static void edit_cursor_to_start(ic_env_t* env, editor_t* eb);
static void edit_cursor_to_end(ic_env_t* env, editor_t* eb);
static void edit_cursor_match_brace(ic_env_t* env, editor_t* eb);
static void edit_backspace(ic_env_t* env, editor_t* eb);
static void edit_delete_char(ic_env_t* env, editor_t* eb);
static void edit_delete_to_end_of_word(ic_env_t* env, editor_t* eb);
static void edit_delete_to_start_of_ws_word(ic_env_t* env, editor_t* eb);
static void edit_delete_to_start_of_word(ic_env_t* env, editor_t* eb);
static void edit_delete_to_start_of_line(ic_env_t* env, editor_t* eb);
static void edit_delete_to_end_of_line(ic_env_t* env, editor_t* eb);
static void edit_swap_char(ic_env_t* env, editor_t* eb);
static void edit_insert_char(ic_env_t* env, editor_t* eb, char c);
static void edit_insert_auto_indented_linefeed(ic_env_t* env, editor_t* eb);
static bool edit_try_expand_abbreviation(ic_env_t* env, editor_t* eb, bool boundary_char_present,
                                         bool modification_started);
static void edit_refresh(ic_env_t* env, editor_t* eb);
static void edit_refresh_hint(ic_env_t* env, editor_t* eb);
static void redraw_prompt_prefix_lines(ic_env_t* env, editor_t* eb);
static bool edit_handle_mouse_click(ic_env_t* env, editor_t* eb, const char* rendered_hint);
static bool edit_mouse_event_to_target_rowcol(ic_env_t* env, editor_t* eb,
                                              const tty_mouse_event_t* mouse_event,
                                              ssize_t* target_row, ssize_t* target_col,
                                              bool* clicked_above_viewport);
static bool edit_key_is_mouse_toggle_binding(const ic_env_t* env, code_t key);
static void edit_set_mouse_reporting_enabled(ic_env_t* env, editor_t* eb, bool enabled);
static void edit_toggle_mouse_reporting(ic_env_t* env, editor_t* eb);
static void edit_reset_mouse_reporting_session(ic_env_t* env, editor_t* eb, bool apply_default);
static bool edit_try_spell_correct_on_enter(ic_env_t* env, editor_t* eb);

static void edit_reset_last_arg_state(editor_t* eb) {
    if (eb == NULL) {
        return;
    }
    eb->last_arg_yank_active = false;
    eb->last_arg_yank_history_idx = 0;
    eb->last_arg_yank_start = 0;
    eb->last_arg_yank_end = 0;
}

static bool key_action_execute(ic_env_t* env, editor_t* eb, ic_key_action_t action, code_t key) {
    switch (action) {
        case IC_KEY_ACTION_NONE:
            return true;
        case IC_KEY_ACTION_COMPLETE:
            edit_generate_completions(env, eb, false);
            return true;
        case IC_KEY_ACTION_HISTORY_SEARCH:
            edit_history_search_with_current_line(env, eb);
            return true;
        case IC_KEY_ACTION_COMMAND_PALETTE:
            edit_command_palette(env, eb);
            return true;
        case IC_KEY_ACTION_HISTORY_PREV:
            if (key == WITH_SHIFT(KEY_UP)) {
                edit_history_prev_with_spell(env, eb);
            } else {
                edit_history_prev(env, eb);
            }
            return true;
        case IC_KEY_ACTION_HISTORY_NEXT:
            edit_history_next(env, eb);
            return true;
        case IC_KEY_ACTION_CLEAR_SCREEN:
            edit_clear_screen(env, eb);
            return true;
        case IC_KEY_ACTION_UNDO:
            edit_undo_restore(env, eb);
            return true;
        case IC_KEY_ACTION_REDO:
            edit_redo_restore(env, eb);
            return true;
        case IC_KEY_ACTION_SHOW_HELP:
            edit_show_help(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_LEFT:
            edit_cursor_left(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE:
            if (!env->completion_auto_menu && eb->pos == sbuf_len(eb->input)) {
                edit_generate_completions(env, eb, false);
            } else {
                edit_cursor_right(env, eb);
            }
            return true;
        case IC_KEY_ACTION_CURSOR_UP:
            edit_cursor_row_up(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_DOWN:
            edit_cursor_row_down(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_LINE_START:
            if (key == KEY_CTRL_A) {
                edit_cursor_ctrl_a(env, eb);
            } else {
                edit_cursor_line_start(env, eb);
            }
            return true;
        case IC_KEY_ACTION_CURSOR_LINE_END:
            if (key == KEY_CTRL_E) {
                edit_cursor_ctrl_e(env, eb);
            } else {
                edit_cursor_line_end(env, eb);
            }
            return true;
        case IC_KEY_ACTION_CURSOR_WORD_PREV:
            edit_cursor_prev_word(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE:
            if (!env->completion_auto_menu && eb->pos == sbuf_len(eb->input)) {
                edit_generate_completions(env, eb, false);
            } else {
                edit_cursor_next_word(env, eb);
            }
            return true;
        case IC_KEY_ACTION_CURSOR_INPUT_START:
            edit_cursor_to_start(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_INPUT_END:
            edit_cursor_to_end(env, eb);
            return true;
        case IC_KEY_ACTION_CURSOR_MATCH_BRACE:
            edit_cursor_match_brace(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_BACKWARD:
            edit_backspace(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_FORWARD:
            edit_delete_char(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_WORD_END:
            edit_delete_to_end_of_word(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_WORD_START_WS:
            edit_delete_to_start_of_ws_word(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_WORD_START:
            edit_delete_to_start_of_word(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_LINE_START:
            edit_delete_to_start_of_line(env, eb);
            return true;
        case IC_KEY_ACTION_DELETE_LINE_END:
            edit_delete_to_end_of_line(env, eb);
            return true;
        case IC_KEY_ACTION_TRANSPOSE_CHARS:
            edit_swap_char(env, eb);
            return true;
        case IC_KEY_ACTION_YANK_LAST_ARG:
            edit_yank_last_arg(env, eb);
            return true;
        case IC_KEY_ACTION_INSERT_NEWLINE:
            if (!env->singleline_only) {
                edit_insert_char(env, eb, '\n');
            }
            return true;
        case IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING:
            edit_toggle_mouse_reporting(env, eb);
            return true;
        default:
            break;
    }
    return false;
}

static bool key_binding_lookup_action(const ic_env_t* env, code_t key,
                                      ic_key_action_t* action_out) {
    if (env == NULL || env->key_binding_count <= 0 || env->key_bindings == NULL) {
        return false;
    }
    for (ssize_t i = 0; i < env->key_binding_count; ++i) {
        const ic_key_binding_entry_t* entry = &env->key_bindings[i];
        if (entry->key == key) {
            if (action_out != NULL) {
                *action_out = entry->action;
            }
            return true;
        }
    }
    return false;
}

static bool edit_key_resets_last_arg_state(const ic_env_t* env, code_t key) {
    if (key == KEY_NONE || key >= KEY_EVENT_BASE) {
        return false;
    }
    ic_key_action_t action = IC_KEY_ACTION__MAX;
    if (key_binding_lookup_action(env, key, &action)) {
        return (action != IC_KEY_ACTION_YANK_LAST_ARG);
    }
    if (key == WITH_ALT('.') || key == WITH_ALT('_')) {
        return false;
    }
    return true;
}

static bool key_binding_execute(ic_env_t* env, editor_t* eb, code_t key) {
    if (env == NULL || env->key_binding_count <= 0) {
        return false;
    }
    ic_key_action_t action = IC_KEY_ACTION__MAX;
    if (key_binding_lookup_action(env, key, &action)) {
        if (action == IC_KEY_ACTION_NONE) {
            return true;
        }
        if (action == IC_KEY_ACTION_RUNOFF) {
            // Call the unhandled key handler directly
            if (env->unhandled_key_handler != NULL) {
                return env->unhandled_key_handler(key, env->unhandled_key_arg);
            }
            return false;
        }
        return key_action_execute(env, eb, action, key);
    }
    return false;
}

//-------------------------------------------------------------
// Main edit line
//-------------------------------------------------------------
static bool insert_initial_input(const char* initial_input, editor_t* eb, size_t cursor_pos,
                                 bool cursor_pos_set);  // defined below

static void edit_set_rendered_hint_snapshot(editor_t* eb, const char* hint_text) {
    if (eb == NULL || eb->mem == NULL) {
        return;
    }

    if (eb->rendered_hint_snapshot != NULL) {
        mem_free(eb->mem, eb->rendered_hint_snapshot);
        eb->rendered_hint_snapshot = NULL;
    }

    if (hint_text != NULL && hint_text[0] != '\0') {
        eb->rendered_hint_snapshot = mem_strdup(eb->mem, hint_text);
    }
}

static char* edit_line(ic_env_t* env, const char* prompt_text,
                       const char* inline_right_text);  // defined at bottom
static bool sbuf_ends_with_newline(stringbuf_t* sbuf);
static bool edit_update_status_message(ic_env_t* env, editor_t* eb);

static int64_t edit_monotonic_time_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        return ((int64_t)now.tv_sec * 1000) + ((int64_t)now.tv_nsec / 1000000);
    }
    return (int64_t)time(NULL) * 1000;
#endif
}

static long edit_milliseconds_until(int64_t deadline_ms) {
    int64_t remaining = deadline_ms - edit_monotonic_time_ms();
    if (remaining <= 0) {
        return 0;
    }
    if (remaining > LONG_MAX) {
        return LONG_MAX;
    }
    return (long)remaining;
}

static int64_t edit_deadline_after(int64_t start_ms, long delay_ms) {
    if (delay_ms <= 0) {
        return 0;
    }
    if (start_ms > INT64_MAX - (int64_t)delay_ms) {
        return INT64_MAX;
    }
    return start_ms + (int64_t)delay_ms;
}

static void edit_note_input_activity(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return;
    }
    eb->last_activity_ms = edit_monotonic_time_ms();
    eb->idle_deadline_ms = edit_deadline_after(eb->last_activity_ms, env->idle_timeout);
}

// Menus run nested input loops, so they must share the editor's idle deadline instead of
// blocking indefinitely. A false return tells the menu to cancel and return to the main loop,
// which then completes the readline with an idle disposition.
static void edit_process_readline_event(ic_env_t* env);

static bool edit_menu_read_key(ic_env_t* env, editor_t* eb, code_t* code) {
    if (env == NULL || eb == NULL || code == NULL || env->tty == NULL) {
        return false;
    }
    while (true) {
        long idle_remaining =
            (env->idle_timeout > 0 ? edit_milliseconds_until(eb->idle_deadline_ms) : -1);
        if (idle_remaining == 0) {
            return false;
        }
        if (tty_read_timeout(env->tty, idle_remaining, code)) {
            if (*code == KEY_EVENT_READLINE) {
                env->readline_event_pending = true;
                edit_process_readline_event(env);
                continue;
            }
            if (*code == KEY_EVENT_STOP || *code == KEY_EVENT_INTERRUPT) {
                // Unwind nested menus before the outer editor handles signals.
                tty_code_pushback(env->tty, *code);
                *code = KEY_ESC;
                return false;
            }
            if (*code != KEY_EVENT_RESIZE) {
                edit_note_input_activity(env, eb);
            }
            return true;
        }
        if (env->idle_timeout > 0 && edit_milliseconds_until(eb->idle_deadline_ms) == 0) {
            return false;
        }
    }
}

ic_private char* ic_editline(ic_env_t* env, const char* prompt_text,
                             const char* inline_right_text) {
    (void)tty_start_raw(env->tty);
    tty_enable_typeahead_capture_mode(env->tty, env->typeahead_enabled);
    term_start_raw(env->term);
    char* line = edit_line(env, prompt_text, inline_right_text);
    const bool idle_timeout = (env->last_readline_disposition == IC_READLINE_DISPOSITION_IDLE);
    term_end_raw(env->term, false);
    tty_end_raw(env->tty);
    if (!idle_timeout) {
        term_writeln(env->term, "");
    }
    // A final redraw callback may have queued another message. The editor has
    // now been released, so drain it without invoking any more render callbacks.
    if (env->notifications != NULL) {
        ic_term_abort_input_region(env);
        term_write(env->term, sbuf_string(env->notifications));
        sbuf_free(env->notifications);
        env->notifications = NULL;
    }
    term_flush(env->term);
    term_set_track_output(env->term, true);
    term_reset_line_state(env->term);
    return line;
}

//-------------------------------------------------------------
// Undo/Redo
//-------------------------------------------------------------

// capture the current edit state
static void editor_capture(editor_t* eb, editstate_t** es) {
    if (!eb->disable_undo) {
        editstate_capture(eb->mem, es, sbuf_string(eb->input), eb->pos);
    }
}

static void editor_undo_capture(editor_t* eb) {
    editor_capture(eb, &eb->undo);
}

static void editor_undo_forget(editor_t* eb) {
    if (eb->disable_undo) {
        return;
    }
    const char* input = NULL;
    ssize_t pos = 0;
    (void)editstate_restore(eb->mem, &eb->undo, &input, &pos);
    mem_free(eb->mem, input);
}

static void editor_restore(editor_t* eb, editstate_t** from, editstate_t** to) {
    if (eb->disable_undo) {
        return;
    }
    if (*from == NULL) {
        return;
    }
    const char* input;
    if (to != NULL) {
        editor_capture(eb, to);
    }
    if (!editstate_restore(eb->mem, from, &input, &eb->pos)) {
        return;
    }
    sbuf_replace(eb->input, input);
    mem_free(eb->mem, input);
    eb->modified = false;
}

static void editor_undo_restore(editor_t* eb, bool with_redo) {
    editor_restore(eb, &eb->undo, (with_redo ? &eb->redo : NULL));
}

static void editor_redo_restore(editor_t* eb) {
    editor_restore(eb, &eb->redo, &eb->undo);
    eb->modified = false;
}

static void editor_start_modify_internal(editor_t* eb, bool preserve_last_arg_state) {
    editor_undo_capture(eb);
    editstate_done(eb->mem, &eb->redo);  // clear redo
    eb->modified = true;
    // Clear history preview when user starts modifying input
    edit_clear_history_preview(eb);
    if (!preserve_last_arg_state) {
        edit_reset_last_arg_state(eb);
    }
}

static void editor_start_modify(editor_t* eb) {
    editor_start_modify_internal(eb, false);
}

static void editor_start_modify_preserve_last_arg(editor_t* eb) {
    editor_start_modify_internal(eb, true);
}

static bool editor_pos_is_at_end(editor_t* eb) {
    return (eb->pos == sbuf_len(eb->input));
}

//-------------------------------------------------------------
// Row/Column width and positioning
//-------------------------------------------------------------

static ssize_t compute_continuation_indent_target(ic_env_t* env, editor_t* eb, ssize_t promptw) {
    ic_unused(eb);
    ssize_t cmarkerw = bbcode_column_width(env->bbcode, env->cprompt_marker);
    if (env->no_multiline_indent) {
        return cmarkerw;
    }
    return (promptw > cmarkerw ? promptw : cmarkerw);
}

static ssize_t estimate_line_number_column_width(const editor_t* eb) {
    ssize_t baseline = (eb->cur_rows > 0 ? eb->cur_rows : 1);
    ssize_t digits = 0;
    ssize_t value = baseline;
    while (value > 0) {
        digits++;
        value /= 10;
    }
    if (digits == 0) {
        digits = 1;
    }
    return digits + 2;  // account for "| " separator
}

static void edit_get_prompt_width(ic_env_t* env, editor_t* eb, bool in_extra, ssize_t* promptw,
                                  ssize_t* cpromptw) {
    if (in_extra) {
        *promptw = 0;
        *cpromptw = 0;
    } else {
        // todo: cache prompt widths
        ssize_t textw = bbcode_column_width(env->bbcode, eb->prompt_text);
        ssize_t markerw = bbcode_column_width(env->bbcode, env->prompt_marker);
        ssize_t base_promptw = markerw + textw;

        ssize_t indent_target = compute_continuation_indent_target(env, eb, base_promptw);
        ssize_t line_prefix_width = indent_target;
        if (line_numbers_enabled(env)) {
            ssize_t cached_width =
                (eb->line_number_column_width > 0 ? eb->line_number_column_width
                                                  : estimate_line_number_column_width(eb));
            if (cached_width > line_prefix_width) {
                line_prefix_width = cached_width;
            }
        }

        *promptw = (eb->replace_prompt_line_with_number ? line_prefix_width : base_promptw);
        *cpromptw = line_prefix_width;

        // Update cached inline right text width

        if (eb->inline_right_text != NULL) {
            eb->inline_right_width = bbcode_column_width(env->bbcode, eb->inline_right_text);
            // Try direct string length as backup
            ssize_t str_len = strlen(eb->inline_right_text);

            // TEMPORARY FIX: If bbcode_column_width returns 0, use estimated
            // visible width
            if (eb->inline_right_width == 0 && str_len > 0) {
                // For now, estimate that our time format [HH:MM:SS] is 10
                // characters
                eb->inline_right_width = 10;
            }
        } else {
            eb->inline_right_width = 0;
        }
    }
}

static ssize_t edit_get_rowcol(ic_env_t* env, editor_t* eb, rowcol_t* rc) {
    ssize_t promptw, cpromptw;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);
    return sbuf_get_rc_at_pos(eb->input, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                              eb->pos, rc);
}

static ssize_t edit_available_terminal_rows(ic_env_t* env, const editor_t* eb) {
    if (env == NULL || env->term == NULL) {
        return 1;
    }

    ssize_t available_rows = term_get_height(env->term);
    if (eb != NULL && eb->prompt_prefix_lines > 0) {
        available_rows -= eb->prompt_prefix_lines;
    }
    return (available_rows > 0 ? available_rows : 1);
}

static ssize_t edit_visible_input_row_count(ic_env_t* env, editor_t* eb, ssize_t input_rows) {
    if (env == NULL || input_rows < 1) {
        return 1;
    }

    const ssize_t available_rows = edit_available_terminal_rows(env, eb);

    return editline_viewport_for(input_rows, 0, input_rows - 1, available_rows,
                                 env->multiline_max_line_count, env->multiline_bottom_line_count, 0)
        .input_row_count;
}

static void edit_set_pos_at_rowcol(ic_env_t* env, editor_t* eb, ssize_t row, ssize_t col) {
    ssize_t promptw, cpromptw;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);
    ssize_t pos = sbuf_get_pos_at_rc(eb->input, eb->termw, promptw, cpromptw,
                                     env->line_wrap_marker_width, row, col);
    if (pos < 0) {
        return;
    }
    eb->pos = pos;
    edit_refresh_hint(env, eb);
}

static ssize_t edit_view_cursor_row(const editor_t* eb) {
    if (eb == NULL || eb->view_rows <= 0) {
        return 0;
    }

    ssize_t cursor_row = eb->cur_row - eb->view_first_row;
    if (cursor_row < 0) {
        cursor_row = 0;
    } else if (cursor_row >= eb->view_input_rows) {
        cursor_row = (eb->view_input_rows > 0 ? eb->view_input_rows - 1 : 0);
    }
    return cursor_row;
}

static bool edit_view_row_to_logical_row(const editor_t* eb, ssize_t view_row,
                                         ssize_t* logical_row) {
    if (eb == NULL || logical_row == NULL || view_row < 0 || view_row >= eb->view_rows) {
        return false;
    }

    if (view_row < eb->view_input_rows) {
        *logical_row = eb->view_first_row + view_row;
    } else {
        *logical_row = eb->input_rows + (view_row - eb->view_input_rows);
    }
    return (*logical_row >= 0 && *logical_row < eb->cur_rows);
}

static bool edit_mouse_event_to_target_rowcol(ic_env_t* env, editor_t* eb,
                                              const tty_mouse_event_t* mouse_event,
                                              ssize_t* target_row, ssize_t* target_col,
                                              bool* clicked_above_viewport) {
    if (env == NULL || eb == NULL || env->term == NULL || mouse_event == NULL ||
        target_row == NULL || target_col == NULL) {
        return false;
    }

    if (clicked_above_viewport != NULL) {
        *clicked_above_viewport = false;
    }

    if (mouse_event->column <= 0 || mouse_event->row <= 0) {
        return false;
    }

    ssize_t cursor_row = 0;
    ssize_t cursor_col = 0;
    bool has_cursor_pos = term_query_cursor_pos(env->term, &cursor_row, &cursor_col);
    if (has_cursor_pos) {
        eb->last_screen_cursor_row = cursor_row;
        eb->last_screen_cursor_col = cursor_col;
        eb->last_screen_cursor_known = true;
    } else if (eb->last_screen_cursor_known) {
        cursor_row = eb->last_screen_cursor_row;
        cursor_col = eb->last_screen_cursor_col;
        has_cursor_pos = true;
    }

    if (!has_cursor_pos) {
        const ssize_t fallback_view_row = mouse_event->row - 1;
        ssize_t fallback_row = 0;
        if (!edit_view_row_to_logical_row(eb, fallback_view_row, &fallback_row)) {
            return false;
        }

        ssize_t fallback_col = mouse_event->column - 1;
        if (fallback_col < 0) {
            fallback_col = 0;
        }

        *target_row = fallback_row;
        *target_col = fallback_col;
        return true;
    }
    ic_unused(cursor_col);

    const ssize_t visible_cursor_row = edit_view_cursor_row(eb);

    ssize_t top_row = cursor_row - visible_cursor_row;
    ssize_t clicked_visible_row = mouse_event->row - top_row;
    if (clicked_visible_row < 0) {
        if (clicked_above_viewport != NULL) {
            *clicked_above_viewport = true;
        }
        return false;
    }

    ssize_t row = 0;
    if (!edit_view_row_to_logical_row(eb, clicked_visible_row, &row)) {
        return false;
    }

    ssize_t col = mouse_event->column - 1;
    if (col < 0) {
        col = 0;
    }

    *target_row = row;
    *target_col = col;
    return true;
}

static bool edit_handle_mouse_click(ic_env_t* env, editor_t* eb, const char* rendered_hint) {
    if (env == NULL || eb == NULL || env->tty == NULL || env->term == NULL) {
        return false;
    }

    tty_mouse_event_t mouse_event;
    if (!tty_get_last_mouse_event(env->tty, &mouse_event)) {
        return false;
    }

    if (mouse_event.action != TTY_MOUSE_ACTION_LEFT_RELEASE) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col_absolute = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &mouse_event, &target_row, &target_col_absolute,
                                           NULL)) {
        return false;
    }

    ssize_t promptw = 0;
    ssize_t cpromptw = 0;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);

    const char* active_hint = rendered_hint;
    if ((active_hint == NULL || active_hint[0] == '\0') && eb->rendered_hint_snapshot != NULL &&
        eb->rendered_hint_snapshot[0] != '\0') {
        active_hint = eb->rendered_hint_snapshot;
    }
    if (!env->completion_auto_menu && (active_hint == NULL || active_hint[0] == '\0') &&
        env->completions != NULL && completions_count(env->completions) > 0) {
        active_hint = completions_get_hint(env->completions, 0, NULL);
    }

    stringbuf_t* display_input = eb->input;
    stringbuf_t* display_input_with_hint = NULL;
    ssize_t display_hint_len = 0;
    const ssize_t display_hint_insert_pos = eb->pos;

    if (active_hint != NULL && active_hint[0] != '\0') {
        display_input_with_hint = sbuf_new(eb->mem);
        if (display_input_with_hint != NULL) {
            sbuf_replace(display_input_with_hint, sbuf_string(eb->input));
            display_hint_len = ic_strlen(active_hint);
            if (display_hint_len > 0) {
                (void)sbuf_insert_at(display_input_with_hint, active_hint, display_hint_insert_pos);
                display_input = display_input_with_hint;
            } else {
                display_hint_len = 0;
            }
        }
    }

    rowcol_t end_rc = {0};
    ssize_t input_rows =
        sbuf_get_rc_at_pos(display_input, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                           sbuf_len(display_input), &end_rc);
    if (target_row >= input_rows) {
        sbuf_free(display_input_with_hint);
        return false;
    }

    ssize_t row_prompt_width = (target_row == 0 ? promptw : cpromptw);
    ssize_t target_col = target_col_absolute - row_prompt_width;
    if (target_col < 0) {
        target_col = 0;
    }

    ssize_t new_pos = sbuf_get_pos_at_rc(display_input, eb->termw, promptw, cpromptw,
                                         env->line_wrap_marker_width, target_row, target_col);
    if (new_pos < 0) {
        sbuf_free(display_input_with_hint);
        return false;
    }

    bool clicked_hint_region = false;
    if (display_input_with_hint != NULL && display_hint_len > 0) {
        const ssize_t display_hint_end_pos = display_hint_insert_pos + display_hint_len;
        if (new_pos > display_hint_insert_pos) {
            if (new_pos <= display_hint_end_pos) {
                clicked_hint_region = true;
                new_pos = display_hint_insert_pos;
            } else {
                new_pos -= display_hint_len;
            }
        }
    }

    sbuf_free(display_input_with_hint);

    if (clicked_hint_region) {
        if (!edit_completion_click_accept_enabled(env)) {
            if (new_pos != eb->pos) {
                eb->pos = new_pos;
                edit_refresh_hint(env, eb);
            }
            return true;
        }

        bool spell_hint = false;
        if (env->completions != NULL && completions_count(env->completions) > 0) {
            const char* source = completions_get_source(env->completions, 0);
            spell_hint = (source != NULL && strcmp(source, "spell") == 0);
        }

        if (active_hint != NULL && active_hint[0] != '\0' && !spell_hint) {
            editor_start_modify(eb);
            ssize_t accepted_pos = sbuf_insert_at(eb->input, active_hint, eb->pos);
            if (accepted_pos >= 0) {
                eb->pos = accepted_pos;
            }
            edit_refresh_hint(env, eb);
        } else {
            edit_generate_completions(env, eb, true);
        }
        return true;
    }

    if (new_pos != eb->pos) {
        eb->pos = new_pos;
        edit_refresh_hint(env, eb);
    }

    return true;
}

static bool edit_pos_is_at_row_end(ic_env_t* env, editor_t* eb) {
    rowcol_t rc;
    (void)edit_get_rowcol(env, eb, &rc);
    return rc.last_on_row;
}

static bool edit_complete(ic_env_t* env, editor_t* eb, ssize_t idx);

static ssize_t edit_find_word_start(const char* input, ssize_t pos) {
    ssize_t start = pos;
    while (start > 0) {
        ssize_t prev = str_prev_ofs(input, start, NULL);
        if (prev <= 0) {
            break;
        }
        if (ic_char_is_separator(input + start - prev, (long)prev)) {
            break;
        }
        start -= prev;
    }
    return start;
}

static bool edit_completion_is_current_word_spell(ic_env_t* env, editor_t* eb, ssize_t index,
                                                  const char** replacement_out,
                                                  ssize_t* word_start_out) {
    if (replacement_out != NULL) {
        *replacement_out = NULL;
    }
    if (word_start_out != NULL) {
        *word_start_out = 0;
    }
    if (env == NULL || eb == NULL || eb->input == NULL || env->completions == NULL ||
        eb->pos <= 0) {
        return false;
    }

    const char* source = completions_get_source(env->completions, index);
    if (source == NULL || strcmp(source, "spell") != 0) {
        return false;
    }

    const char* input = sbuf_string(eb->input);
    if (input == NULL) {
        return false;
    }

    ssize_t input_len = ic_strlen(input);
    ssize_t next_len = str_next_ofs(input, input_len, eb->pos, NULL);
    if (next_len > 0 && !ic_char_is_separator(input + eb->pos, (long)next_len)) {
        return false;
    }

    ssize_t word_start = edit_find_word_start(input, eb->pos);
    if (word_start < 0 || word_start >= eb->pos) {
        return false;
    }

    const char* replacement = NULL;
    ssize_t replacement_start = 0;
    if (!completions_get_apply_range(env->completions, index, input, eb->pos, &replacement,
                                     &replacement_start, NULL) ||
        replacement == NULL || replacement[0] == '\0' || replacement_start != word_start) {
        return false;
    }

    if (replacement_out != NULL) {
        *replacement_out = replacement;
    }
    if (word_start_out != NULL) {
        *word_start_out = word_start;
    }
    return true;
}

static inline char ascii_tolower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static size_t levenshtein_casefold(alloc_t* mem, const char* left, const char* right) {
    if (left == NULL || right == NULL) {
        return SIZE_MAX;
    }
    size_t len_left = strlen(left);
    size_t len_right = strlen(right);
    if (len_left == 0) {
        return len_right;
    }
    if (len_right == 0) {
        return len_left;
    }

    size_t* prev = mem_malloc_tp_n(mem, size_t, len_right + 1);
    size_t* curr = mem_malloc_tp_n(mem, size_t, len_right + 1);
    if (prev == NULL || curr == NULL) {
        mem_free(mem, prev);
        mem_free(mem, curr);
        return SIZE_MAX;
    }

    for (size_t j = 0; j <= len_right; ++j) {
        prev[j] = j;
    }

    for (size_t i = 1; i <= len_left; ++i) {
        curr[0] = i;
        char cl = ascii_tolower_char(left[i - 1]);
        for (size_t j = 1; j <= len_right; ++j) {
            char cr = ascii_tolower_char(right[j - 1]);
            size_t cost = (cl == cr ? 0 : 1);
            size_t deletion = prev[j] + 1;
            size_t insertion = curr[j - 1] + 1;
            size_t substitution = prev[j - 1] + cost;
            size_t best = deletion;
            if (insertion < best) {
                best = insertion;
            }
            if (substitution < best) {
                best = substitution;
            }
            curr[j] = best;
        }
        size_t* tmp = prev;
        prev = curr;
        curr = tmp;
    }

    size_t result = prev[len_right];
    mem_free(mem, prev);
    mem_free(mem, curr);
    return result;
}

static size_t edit_spell_threshold(size_t left_len, size_t right_len) {
    size_t max_len = (left_len > right_len ? left_len : right_len);
    if (max_len <= 2) {
        return 1;
    }
    if (max_len <= 4) {
        return 1;
    }
    if (max_len <= 6) {
        return 2;
    }
    return max_len / 2;
}

static ssize_t edit_best_spell_candidate(ic_env_t* env, const char* original_word, ssize_t count,
                                         const char* required_source) {
    ssize_t best_index = -1;
    size_t best_distance = SIZE_MAX;
    long best_length_diff = LONG_MAX;
    const ssize_t original_len = ic_strlen(original_word);
    ssize_t best_length = 0;
    for (ssize_t i = 0; i < count; ++i) {
        if (required_source != NULL) {
            const char* source = completions_get_source(env->completions, i);
            if (source == NULL || strcmp(source, required_source) != 0) {
                continue;
            }
        }
        const char* replacement = completions_get_replacement(env->completions, i);
        if (replacement == NULL || *replacement == '\0') {
            continue;
        }
        const size_t distance = levenshtein_casefold(env->mem, original_word, replacement);
        if (distance == SIZE_MAX) {
            continue;
        }
        const ssize_t length = ic_strlen(replacement);
        const long length_diff = labs((long)length - (long)original_len);
        if (distance < best_distance ||
            (distance == best_distance && length_diff < best_length_diff)) {
            best_index = i;
            best_distance = distance;
            best_length_diff = length_diff;
            best_length = length;
        }
    }
    return best_index >= 0 &&
                   best_distance <= edit_spell_threshold((size_t)original_len, (size_t)best_length)
               ? best_index
               : -1;
}

static bool edit_try_spell_correct(ic_env_t* env, editor_t* eb) {
    if (!env->spell_correct) {
        return false;
    }

    const char* input = sbuf_string(eb->input);
    if (input == NULL) {
        return false;
    }
    ssize_t pos = eb->pos;
    if (pos <= 0) {
        return false;
    }

    ssize_t prev = str_prev_ofs(input, pos, NULL);
    if (prev <= 0) {
        return false;
    }
    if (ic_char_is_separator(input + pos - prev, (long)prev)) {
        return false;
    }

    ssize_t word_start = edit_find_word_start(input, pos);
    if (word_start < 0 || word_start >= pos) {
        return false;
    }

    ssize_t word_len = pos - word_start;
    char* original_word = mem_strndup(env->mem, input + word_start, word_len);
    if (original_word == NULL) {
        return false;
    }

    editor_start_modify(eb);
    sbuf_delete_from_to(eb->input, word_start, pos);
    eb->pos = word_start;

    ssize_t candidate_count = completions_generate(env, env->completions, sbuf_string(eb->input),
                                                   eb->pos, IC_MAX_COMPLETIONS_TO_TRY);
    if (candidate_count <= 0) {
        editor_undo_restore(eb, false);
        completions_clear(env->completions);
        mem_free(env->mem, original_word);
        return false;
    }

    const ssize_t best_index = edit_best_spell_candidate(env, original_word, candidate_count, NULL);
    const bool applied = best_index >= 0 && edit_complete(env, eb, best_index);

    if (!applied) {
        editor_undo_restore(eb, false);
    }
    completions_clear(env->completions);
    mem_free(env->mem, original_word);
    return applied;
}

static bool edit_try_spell_correct_on_enter(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return false;
    }
    if (!env->spell_correct || !env->spell_correct_on_enter) {
        return false;
    }

    const char* input = sbuf_string(eb->input);
    if (input == NULL || eb->pos <= 0) {
        return false;
    }

    ssize_t prev = str_prev_ofs(input, eb->pos, NULL);
    if (prev <= 0) {
        return false;
    }
    if (ic_char_is_separator(input + eb->pos - prev, (long)prev)) {
        return false;
    }

    ssize_t count =
        completions_generate(env, env->completions, input, eb->pos, IC_MAX_COMPLETIONS_TO_TRY);
    bool applied = false;
    if (count == 1 && completions_all_sources_equal(env->completions, "spell")) {
        applied = edit_complete(env, eb, 0);
    }
    completions_clear(env->completions);
    return applied;
}

// Helper function to extract the last line from a multi-line prompt
static char* extract_last_prompt_line(alloc_t* mem, const char* prompt_text) {
    if (prompt_text == NULL) {
        return mem_strdup(mem, "");
    }

    // Find the last newline in the prompt
    const char* last_newline = strrchr(prompt_text, '\n');
    if (last_newline == NULL) {
        // No newlines, return the whole prompt
        return mem_strdup(mem, prompt_text);
    }

    // Return everything after the last newline
    return mem_strdup(mem, last_newline + 1);
}

// Helper function to print all but the last line of a multi-line prompt
static ssize_t print_prompt_prefix_lines(ic_env_t* env, editor_t* eb, const char* prompt_text) {
    if (env == NULL || eb == NULL) {
        return 0;
    }

    if (eb->prompt_prefix_text != NULL) {
        mem_free(env->mem, eb->prompt_prefix_text);
        eb->prompt_prefix_text = NULL;
    }

    if (prompt_text == NULL) {
        return 0;
    }

    const char* last_newline = strrchr(prompt_text, '\n');
    if (last_newline == NULL) {
        // No newlines, nothing to print
        return 0;
    }

    ssize_t prefix_length = to_ssize_t(last_newline - prompt_text + 1);  // +1 to include newline
    if (prefix_length <= 0) {
        return 0;
    }

    char* prefix = mem_strndup(env->mem, prompt_text, prefix_length);
    if (prefix == NULL) {
        return 0;
    }

    eb->prompt_prefix_text = prefix;

    // Print the prefix lines directly to the terminal
    ic_term_mark_prompt_start(env, false);
    bbcode_print(env->bbcode, prefix);
    ic_term_mark_input_start(env);

    // Count how many lines we emitted (number of newline characters)
    ssize_t lines = 0;
    for (ssize_t i = 0; i < prefix_length; ++i) {
        if (prefix[i] == '\n') {
            lines++;
        }
    }

    return lines;
}

static void redraw_prompt_prefix_lines(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return;
    }
    if (eb->prompt_prefix_text == NULL || eb->prompt_prefix_lines <= 0) {
        return;
    }
    term_start_of_line(env->term);
    ic_term_mark_prompt_start(env, false);
    bbcode_print(env->bbcode, eb->prompt_prefix_text);
    ic_term_mark_input_start(env);
}

static void format_line_number_prompt(char* buffer, size_t buffer_size, ssize_t row,
                                      ssize_t cursor_row, bool relative) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    if (relative) {
        if (cursor_row < 0) {
            (void)snprintf(buffer, buffer_size, "%zd| ", row + 1);
            return;
        }
        ssize_t diff = (row >= cursor_row ? row - cursor_row : cursor_row - row);
        if (diff == 0) {
            // current line number
            (void)snprintf(buffer, buffer_size, "%zd| ", row + 1);
        } else {
            (void)snprintf(buffer, buffer_size, "%zd| ", diff);
        }
    } else {
        (void)snprintf(buffer, buffer_size, "%zd| ", row + 1);
    }
}

static void edit_write_prompt(ic_env_t* env, editor_t* eb, ssize_t row, bool in_extra,
                              ssize_t cursor_row, ssize_t logical_line, ssize_t cursor_logical_line,
                              bool is_continuation_row) {
    if (in_extra) {
        return;
    }
    ic_term_mark_prompt_start(env, row > 0 || is_continuation_row);
    const bool line_numbers_active = line_numbers_enabled(env);
    const bool row_uses_prompt_text = (row == 0 && !eb->replace_prompt_line_with_number);
    const bool row_uses_line_numbers =
        (!in_extra && line_numbers_active && (row > 0 || eb->replace_prompt_line_with_number));

    bbcode_style_open(env->bbcode, "ic-prompt");
    if (row_uses_prompt_text) {
        // regular prompt text
        bbcode_print(env->bbcode, eb->prompt_text);
    } else if (row_uses_line_numbers) {
        // show line numbers for multiline input (row 0 or continuation rows)
        bbcode_style_close(env->bbcode, NULL);

        ssize_t textw = bbcode_column_width(env->bbcode, eb->prompt_text);
        ssize_t markerw = bbcode_column_width(env->bbcode, env->prompt_marker);
        ssize_t promptw = markerw + textw;
        ssize_t indent_target = compute_continuation_indent_target(env, eb, promptw);
        ssize_t desired_width =
            (eb->line_number_column_width > indent_target ? eb->line_number_column_width
                                                          : indent_target);

        if (!is_continuation_row) {
            const bool highlight_line = (cursor_row >= 0 && env->highlight_current_line_number &&
                                         logical_line == cursor_logical_line);
            const char* style = (highlight_line ? "ic-linenumber-current" : "ic-linenumbers");

            bbcode_style_open(env->bbcode, style);

            char line_number_str[16];
            format_line_number_prompt(line_number_str, sizeof(line_number_str), logical_line,
                                      cursor_logical_line, env->relative_line_numbers);
            ssize_t line_number_width = (ssize_t)strlen(line_number_str);
            if (line_number_width > desired_width) {
                desired_width = line_number_width;
            }

            ssize_t leading_spaces = desired_width - line_number_width;
            if (leading_spaces > 0) {
                term_write_repeat(env->term, " ", leading_spaces);
            }

            bbcode_print(env->bbcode, line_number_str);
            bbcode_style_close(env->bbcode, NULL);
        } else {
            term_write_repeat(env->term, " ", desired_width);
        }

        if (desired_width > eb->line_number_column_width) {
            eb->line_number_column_width = desired_width;
        }

        bbcode_style_open(env->bbcode, "ic-prompt");
    } else if (!env->no_multiline_indent) {
        ic_emit_continuation_indent(env, eb->prompt_text);
    }
    // the marker (skip for line numbers since we include our own separator)
    if ((row == 0 && !eb->replace_prompt_line_with_number) || !line_numbers_active) {
        bbcode_print(env->bbcode, (row == 0 ? env->prompt_marker : env->cprompt_marker));
    }
    bbcode_style_close(env->bbcode, NULL);
    ic_term_mark_input_start(env);
}

static ssize_t edit_decode_codepoint(const char* text, ssize_t len, ssize_t offset,
                                     unicode_t* code_out) {
    if (text == NULL || len <= 0 || offset >= len) {
        return 0;
    }
    ssize_t char_len = 0;
    unicode_t code = unicode_from_qutf8((const uint8_t*)text + offset, len - offset, &char_len);
    if (char_len <= 0 || offset + char_len > len) {
        char_len = 1;
        code = (uint8_t)text[offset];
    }
    if (code_out != NULL) {
        *code_out = code;
    }
    return char_len;
}

static void edit_write_row_text(ic_env_t* env, const char* text, ssize_t len, const attr_t* attrs,
                                bool in_extra) {
    if (env == NULL || text == NULL || len <= 0) {
        return;
    }

    if (!env->show_whitespace_characters || in_extra) {
        if (attrs == NULL) {
            term_write_n(env->term, text, len);
        } else {
            term_write_formatted_n(env->term, text, attrs, len);
        }
        return;
    }

    const char* marker = ic_env_get_whitespace_marker(env);
    ssize_t marker_len = ic_strlen(marker);
    if (marker_len <= 0) {
        marker = " ";
        marker_len = 1;
    }

    const attr_t whitespace_attr = bbcode_style(env->bbcode, "ic-whitespace-char");
    const bool has_whitespace_style = !attr_is_none(whitespace_attr);
    const attr_t hint_attr = bbcode_style(env->bbcode, "ic-hint");

    if (attrs == NULL) {
        attr_t default_attr = attr_none();
        bool whitespace_active = false;
        if (has_whitespace_style) {
            term_start_raw(env->term);
            default_attr = term_get_attr(env->term);
        }
        ssize_t offset = 0;
        while (offset < len) {
            unicode_t code = 0;
            ssize_t char_len = edit_decode_codepoint(text, len, offset, &code);
            if (char_len <= 0) {
                break;
            }

            if (code == ' ') {
                if (has_whitespace_style && !whitespace_active) {
                    term_set_attr(env->term, attr_update_with(default_attr, whitespace_attr));
                    whitespace_active = true;
                }
                term_write_n(env->term, marker, marker_len);
            } else {
                if (has_whitespace_style && whitespace_active) {
                    term_set_attr(env->term, default_attr);
                    whitespace_active = false;
                }
                term_write_n(env->term, text + offset, char_len);
            }
            offset += char_len;
        }
        if (has_whitespace_style) {
            term_set_attr(env->term, default_attr);
        }
        return;
    }

    term_start_raw(env->term);
    attr_t default_attr = term_get_attr(env->term);
    attr_t current_attr = attr_none();
    bool whitespace_active = false;
    attr_t whitespace_base_attr = attr_none();
    ssize_t offset = 0;
    while (offset < len) {
        unicode_t code = 0;
        ssize_t char_len = edit_decode_codepoint(text, len, offset, &code);
        if (char_len <= 0) {
            break;
        }

        attr_t attr = attrs[offset];
        attr_t base_attr = attr_update_with(default_attr, attr);
        if (!attr_is_eq(current_attr, attr)) {
            term_set_attr(env->term, base_attr);
            current_attr = attr;
            whitespace_active = false;
        }

        bool is_hint = attr_is_eq(attr, hint_attr);

        if (code == ' ' && !is_hint) {
            if (has_whitespace_style &&
                (!whitespace_active || !attr_is_eq(whitespace_base_attr, base_attr))) {
                term_set_attr(env->term, attr_update_with(base_attr, whitespace_attr));
                whitespace_active = true;
                whitespace_base_attr = base_attr;
            }

            term_write_n(env->term, marker, marker_len);
        } else {
            if (has_whitespace_style && whitespace_active) {
                term_set_attr(env->term, base_attr);
                whitespace_active = false;
            }
            term_write_n(env->term, text + offset, char_len);
        }
        offset += char_len;
    }
    term_set_attr(env->term, default_attr);
}

static stringbuf_t* edit_ensure_extra_buffer(editor_t* eb, stringbuf_t* extra) {
    if (extra == NULL && eb != NULL) {
        extra = sbuf_new(eb->mem);
    }
    return extra;
}

static stringbuf_t* edit_append_extra_block(ic_env_t* env, editor_t* eb, stringbuf_t* extra,
                                            stringbuf_t* block) {
    if (env == NULL || eb == NULL || block == NULL || sbuf_len(block) <= 0) {
        return extra;
    }

    extra = edit_ensure_extra_buffer(eb, extra);
    if (extra == NULL) {
        return NULL;
    }

    if (sbuf_len(extra) > 0 && !sbuf_ends_with_newline(extra)) {
        bbcode_append(env->bbcode, "\n", extra, eb->attrs_extra);
    }

    bbcode_append(env->bbcode, sbuf_string(block), extra, eb->attrs_extra);
    return extra;
}

//-------------------------------------------------------------
// Refresh
//-------------------------------------------------------------

typedef struct refresh_info_s {
    ic_env_t* env;
    editor_t* eb;
    attrbuf_t* attrs;
    bool in_extra;
    ssize_t first_row;
    ssize_t last_row;
    ssize_t cursor_row;
    ssize_t logical_line;
    ssize_t cursor_logical_line;
    bool continuation_row;
    bool has_following_row;
} refresh_info_t;

static void edit_render_inline_right_prompt(refresh_info_t* info, ssize_t row, ssize_t row_len) {
    if (info == NULL || info->eb == NULL || info->eb->inline_right_text == NULL) {
        return;
    }

    ssize_t promptw = 0;
    ssize_t cpromptw = 0;
    edit_get_prompt_width(info->env, info->eb, info->in_extra, &promptw, &cpromptw);

    ssize_t active_promptw = (row == 0 ? promptw : cpromptw);
    ssize_t current_pos = active_promptw + row_len;
    ssize_t right_text_width = info->eb->inline_right_width;
    if (right_text_width <= 0 && info->eb->inline_right_text[0] != '\0') {
        right_text_width = (ssize_t)strlen(info->eb->inline_right_text);
    }

    term_clear_to_end_of_line(info->env->term);

    if (right_text_width <= 0) {
        return;
    }

    ssize_t terminal_width = info->eb->termw;
    if (terminal_width <= current_pos + right_text_width + 1) {
        return;
    }

    ssize_t spaces_needed = terminal_width - current_pos - right_text_width;
    if (spaces_needed < 1) {
        spaces_needed = 1;
    }

    ic_term_mark_prompt_start(info->env, row > 0);
    term_write_repeat(info->env->term, " ", spaces_needed);

    const char* text_to_write = info->eb->inline_right_text;
    const char* time_start = NULL;
    for (const char* p = text_to_write; *p; ++p) {
        if (*p == '[' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] == ':' &&
            p[4] >= '0' && p[4] <= '9' && p[5] >= '0' && p[5] <= '9' && p[6] == ':' &&
            p[7] >= '0' && p[7] <= '9' && p[8] >= '0' && p[8] <= '9' && p[9] == ']') {
            time_start = p;
            break;
        }
    }

    if (time_start != NULL) {
        term_write_n(info->env->term, time_start, 10);
    } else {
        bbcode_print(info->env->bbcode, text_to_write);
    }

    ic_term_mark_input_start(info->env);
    term_flush(info->env->term);
}

static bool edit_refresh_rows_iter(const char* s, ssize_t row, ssize_t row_start, ssize_t row_len,
                                   ssize_t startw, bool is_wrap, const void* arg, void* res) {
    ic_unused(res);
    refresh_info_t* info = (refresh_info_t*)arg;
    term_t* term = info->env->term;

    // debug_msg("edit: line refresh: row %zd, len: %zd\n", row, row_len);
    if (row > info->last_row) {
        return true;  // should not occur
    }

    const bool should_render = (row >= info->first_row);
    const bool row_is_continuation = (!info->in_extra ? info->continuation_row : false);
    const ssize_t logical_line = info->logical_line;
    const ssize_t cursor_line = info->cursor_logical_line;

    if (should_render) {
        // term_clear_line(term);
        edit_write_prompt(info->env, info->eb, row, info->in_extra, info->cursor_row, logical_line,
                          cursor_line, row_is_continuation);

        //' write output
        const bool use_attrs =
            !(info->env->no_highlight && info->env->no_bracematch) && info->attrs != NULL;
        const attr_t* row_attrs = NULL;
        if (use_attrs) {
            const attr_t* attrs = attrbuf_attrs(info->attrs, row_start + row_len);
            if (attrs != NULL) {
                row_attrs = attrs + row_start;
            }
        }
        edit_write_row_text(info->env, s + row_start, row_len, row_attrs, info->in_extra);

        ssize_t inline_right_row = 0;
        if (info->env->inline_right_prompt_follows_cursor && info->cursor_row >= 0) {
            inline_right_row = info->cursor_row;
        }
        const bool has_following_row = (row < info->last_row || info->has_following_row);
        const bool show_wrap_marker =
            (has_following_row && is_wrap && info->env->line_wrap_marker_width > 0 &&
             (tty_is_utf8(info->env->tty) || (uint8_t)info->env->line_wrap_marker[0] < 0x80));
        // A terminal with delayed wrapping keeps its cursor on the last cell of a full
        // row. Erasing from there would erase the last input character or wrap marker.
        const bool row_fills_terminal =
            (startw + str_column_width_n(s + row_start, row_len) +
                 (show_wrap_marker ? info->env->line_wrap_marker_width : 0) >=
             info->eb->termw);
        const bool should_attempt_inline_right =
            (!info->in_extra && info->eb->inline_right_text != NULL && row == inline_right_row &&
             !row_fills_terminal);

        if (should_attempt_inline_right) {
            edit_render_inline_right_prompt(info, row, row_len);
        }

        // write line ending
        if (has_following_row) {
            if (show_wrap_marker) {
                ic_term_mark_prompt_start(info->env, true);
                bbcode_style_open(info->env->bbcode, "ic-diminish");
                term_write(term, info->env->line_wrap_marker);
                bbcode_style_close(info->env->bbcode, NULL);
                ic_term_mark_input_start(info->env);
            }
            if (!should_attempt_inline_right && !row_fills_terminal) {
                term_clear_to_end_of_line(term);
            }
            term_writeln(term, "");
        } else {
            if (!should_attempt_inline_right && !row_fills_terminal) {
                term_clear_to_end_of_line(term);
            }
        }
    }

    if (!info->in_extra) {
        if (is_wrap) {
            info->continuation_row = true;
        } else {
            info->continuation_row = false;
            info->logical_line++;
        }
    }

    return (row >= info->last_row);
}

static void edit_refresh_rows(ic_env_t* env, editor_t* eb, stringbuf_t* input, attrbuf_t* attrs,
                              ssize_t promptw, ssize_t cpromptw, bool in_extra, ssize_t first_row,
                              ssize_t last_row, bool has_following_row, ssize_t cursor_row,
                              ssize_t cursor_logical_line) {
    if (input == NULL) {
        return;
    }
    refresh_info_t info;
    info.env = env;
    info.eb = eb;
    info.attrs = attrs;
    info.in_extra = in_extra;
    info.first_row = first_row;
    info.last_row = last_row;
    info.cursor_row = cursor_row;
    info.logical_line = 0;
    info.cursor_logical_line = cursor_logical_line;
    info.continuation_row = false;
    info.has_following_row = has_following_row;
    (void)sbuf_for_each_row(input, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                            &edit_refresh_rows_iter, &info, NULL);
}

static bool sbuf_ends_with_newline(stringbuf_t* sbuf) {
    ssize_t len = sbuf_len(sbuf);
    if (len <= 0) {
        return false;
    }
    return (sbuf_char_at(sbuf, len - 1) == '\n');
}

static ssize_t count_logical_lines(stringbuf_t* sbuf) {
    if (sbuf == NULL) {
        return 1;
    }
    ssize_t len = sbuf_len(sbuf);
    if (len <= 0) {
        return 1;
    }
    const char* data = sbuf_string(sbuf);
    if (data == NULL) {
        return 1;
    }
    ssize_t lines = 1;
    for (ssize_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

static ssize_t logical_line_at_pos(stringbuf_t* sbuf, ssize_t pos) {
    if (sbuf == NULL) {
        return 0;
    }
    ssize_t len = sbuf_len(sbuf);
    if (len <= 0) {
        return 0;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos > len) {
        pos = len;
    }
    const char* data = sbuf_string(sbuf);
    if (data == NULL) {
        return 0;
    }
    ssize_t line = 0;
    for (ssize_t i = 0; i < pos; ++i) {
        if (data[i] == '\n') {
            line++;
        }
    }
    return line;
}

static bool edit_current_line_is_empty(editor_t* eb) {
    if (eb == NULL || eb->input == NULL) {
        return true;
    }

    ssize_t start = sbuf_find_line_start(eb->input, eb->pos);
    ssize_t end = sbuf_find_line_end(eb->input, eb->pos);
    if (start < 0 || end < 0) {
        return (sbuf_len(eb->input) <= 0);
    }

    return (end <= start);
}

static void edit_refresh(ic_env_t* env, editor_t* eb) {
    // Repainting can erase a terminal's native selection during the capture handoff.
    if (eb->mouse_terminal_selection_suspended) {
        return;
    }
    eb->replace_prompt_line_with_number = prompt_line_should_use_line_numbers(env, eb);
    edit_set_rendered_hint_snapshot(eb, (sbuf_len(eb->hint) > 0 ? sbuf_string(eb->hint) : NULL));
    // calculate the new cursor row and total rows needed
    ssize_t promptw, cpromptw;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);

    if (eb->attrs != NULL) {
        highlight(env->mem, env->bbcode, sbuf_string(eb->input), eb->attrs,
                  (env->no_highlight ? NULL : env->highlighter), env->highlighter_arg);
    }

    // highlight matching braces
    if (eb->attrs != NULL && !env->no_bracematch) {
        highlight_match_braces(
            sbuf_string(eb->input), eb->attrs, eb->pos, ic_env_get_match_braces(env),
            bbcode_style(env->bbcode, "ic-bracematch"), bbcode_style(env->bbcode, "ic-error"));
    }

    // insert hint
    if (sbuf_len(eb->hint) > 0) {
        if (eb->attrs != NULL) {
            attrbuf_insert_at(eb->attrs, eb->pos, sbuf_len(eb->hint),
                              bbcode_style(env->bbcode, "ic-hint"));
        }
        (void)sbuf_insert_at(eb->input, sbuf_string(eb->hint), eb->pos);
    }

    // render extra (like a completion menu) and status message
    stringbuf_t* extra = NULL;
    const bool menu_active = (sbuf_len(eb->extra) > 0);

    if (!menu_active && sbuf_len(eb->status) > 0) {
        extra = sbuf_new(eb->mem);
        if (extra != NULL) {
            bbcode_append(env->bbcode, sbuf_string(eb->status), extra, eb->attrs_extra);
        }
    }

    if (sbuf_len(eb->hint_help) > 0) {
        extra = edit_append_extra_block(env, eb, extra, eb->hint_help);
    }

    if (menu_active) {
        extra = edit_append_extra_block(env, eb, extra, eb->extra);
    }

    // calculate rows and row/col position (account for dynamic line number width)
    rowcol_t rc = {0};
    rowcol_t rc_extra = {0};
    ssize_t rows_input = 0;
    ssize_t rows_extra = 0;
    ssize_t rows = 0;
    ssize_t indent_target = compute_continuation_indent_target(env, eb, promptw);
    int layout_adjustments = 0;
    ssize_t cursor_row_for_display = 0;
    ssize_t logical_line_count = count_logical_lines(eb->input);
    ssize_t cursor_logical_line = logical_line_at_pos(eb->input, eb->pos);
    if (cursor_logical_line >= logical_line_count) {
        cursor_logical_line = logical_line_count - 1;
    }
    ssize_t cursor_line_for_display = 0;

    while (true) {
        rc = (rowcol_t){0};
        rows_input = sbuf_get_rc_at_pos(eb->input, eb->termw, promptw, cpromptw,
                                        env->line_wrap_marker_width, eb->pos, &rc);

        if (extra != NULL) {
            rc_extra = (rowcol_t){0};
            rows_extra = sbuf_get_rc_at_pos(extra, eb->termw, 0, 0, env->line_wrap_marker_width,
                                            0 /*pos*/, &rc_extra);
        } else {
            rows_extra = 0;
        }

        rows = rows_input + rows_extra;
        cursor_row_for_display = (eb->force_linear_line_numbers ? -1 : rc.row);
        cursor_line_for_display = (eb->force_linear_line_numbers ? -1 : cursor_logical_line);

        if (line_numbers_enabled(env)) {
            ssize_t max_line_number_width = 0;
            if (logical_line_count > 0) {
                char line_number_str[16];
                format_line_number_prompt(line_number_str, sizeof(line_number_str), 0,
                                          cursor_line_for_display, env->relative_line_numbers);
                max_line_number_width = (ssize_t)strlen(line_number_str);

                format_line_number_prompt(line_number_str, sizeof(line_number_str),
                                          logical_line_count - 1, cursor_line_for_display,
                                          env->relative_line_numbers);
                ssize_t last_width = (ssize_t)strlen(line_number_str);
                if (last_width > max_line_number_width) {
                    max_line_number_width = last_width;
                }
            }

            ssize_t desired_cpromptw =
                (max_line_number_width > indent_target ? max_line_number_width : indent_target);

            if (desired_cpromptw != cpromptw) {
                cpromptw = desired_cpromptw;
                if (++layout_adjustments > 4) {
                    break;
                }
                continue;
            }

            eb->line_number_column_width = desired_cpromptw;
        } else {
            eb->line_number_column_width = 0;
        }

        break;
    }

    debug_msg(
        "edit: refresh: rows %zd, cursor: %zd,%zd (previous rows %zd, cursor "
        "row "
        "%zd)\n",
        rows, rc.row, rc.col, eb->cur_rows, eb->cur_row);

    // Prefix rows are rendered separately above this viewport. Keep the complete physical editor
    // (prefix, input, and helper rows) within the terminal's row count.
    const ssize_t visible_termh = edit_available_terminal_rows(env, eb);

    const editline_viewport_t viewport = editline_viewport_for(
        rows_input, rows_extra, rc.row, visible_termh, env->multiline_max_line_count,
        env->multiline_bottom_line_count, eb->view_first_row);
    const ssize_t first_input_row = viewport.input_first_row;
    const ssize_t last_input_row = first_input_row + viewport.input_row_count - 1;
    const ssize_t view_rows = viewport.input_row_count + viewport.extra_row_count;
    const ssize_t visible_cursor_row = rc.row - first_input_row;
    assert(viewport.input_row_count >= 1);
    assert(visible_cursor_row >= 0 && visible_cursor_row < viewport.input_row_count);
    assert(view_rows >= 1 && view_rows <= visible_termh);

    // reduce flicker
    buffer_mode_t bmode = term_set_buffer_mode(env->term, BUFFERED);

    // Back up from the cursor's physical row in the previous viewport. Logical row counts can be
    // much larger than the number of rows currently present on screen.
    const ssize_t previous_visible_cursor_row = edit_view_cursor_row(eb);
    term_start_of_line(env->term);
    term_up(env->term, previous_visible_cursor_row);

    // Render the input viewport and then any helper/menu rows. These regions are intentionally
    // non-contiguous in the logical layout when input rows above or below the cursor are hidden.
    edit_refresh_rows(env, eb, eb->input, eb->attrs, promptw, cpromptw, false, first_input_row,
                      last_input_row, viewport.extra_row_count > 0, cursor_row_for_display,
                      cursor_line_for_display);
    if (viewport.extra_row_count > 0) {
        assert(extra != NULL);
        edit_refresh_rows(env, eb, extra, eb->attrs_extra, 0, 0, true, 0,
                          viewport.extra_row_count - 1, false, cursor_row_for_display, -1);
    }

    // overwrite trailing rows we do not use anymore
    ssize_t rrows = view_rows;
    if (rrows < visible_termh && view_rows < eb->view_rows) {
        ssize_t clear = eb->view_rows - view_rows;
        while (rrows < visible_termh && clear > 0) {
            clear--;
            rrows++;
            term_writeln(env->term, "");
            term_clear_line(env->term);
        }
    }

    // move cursor back to edit position
    term_start_of_line(env->term);
    term_up(env->term, rrows - 1 - visible_cursor_row);

    // Calculate the actual prompt width for the current row
    ssize_t actual_prompt_width;
    if (rc.row == 0) {
        actual_prompt_width = promptw;
    } else if (line_numbers_enabled(env)) {
        bool cursor_row_is_continuation = false;
        ssize_t input_len = sbuf_len(eb->input);
        if (rc.row_start > 0 && rc.row_start <= input_len) {
            cursor_row_is_continuation = (sbuf_char_at(eb->input, rc.row_start - 1) != '\n');
        }

        actual_prompt_width = indent_target;
        if (!cursor_row_is_continuation) {
            char line_number_str[16];
            format_line_number_prompt(line_number_str, sizeof(line_number_str), cursor_logical_line,
                                      cursor_line_for_display, env->relative_line_numbers);
            ssize_t line_number_width = (ssize_t)strlen(line_number_str);
            if (line_number_width > actual_prompt_width) {
                actual_prompt_width = line_number_width;
            }
        }
        if (eb->line_number_column_width > actual_prompt_width) {
            actual_prompt_width = eb->line_number_column_width;
        }
    } else {
        actual_prompt_width = cpromptw;
    }

    term_right(env->term, rc.col + actual_prompt_width);

    // and refresh
    term_flush(env->term);

    // stop buffering
    (void)term_set_buffer_mode(env->term, bmode);

    // restore input by removing the hint
    sbuf_delete_at(eb->input, eb->pos, sbuf_len(eb->hint));
    sbuf_delete_at(eb->extra, 0, sbuf_len(eb->hint_help));
    attrbuf_clear(eb->attrs);
    attrbuf_clear(eb->attrs_extra);
    sbuf_free(extra);

    // update previous
    eb->cur_rows = rows;
    eb->input_rows = rows_input;
    eb->mouse_terminal_selection_extra = (!menu_active && rows_extra > 0);
    eb->cur_row = rc.row;
    eb->view_first_row = first_input_row;
    eb->view_input_rows = viewport.input_row_count;
    eb->view_rows = view_rows;
    eb->force_linear_line_numbers = false;
}

// clear current output
static void edit_clear(ic_env_t* env, editor_t* eb) {
    const ssize_t clear_rows = (eb->view_rows > 0 ? eb->view_rows : 1);
    const ssize_t visible_cursor_row = edit_view_cursor_row(eb);
    term_attr_reset(env->term);
    term_up(env->term, visible_cursor_row);

    // overwrite all rows
    for (ssize_t i = 0; i < clear_rows; i++) {
        term_clear_line(env->term);
        term_writeln(env->term, "");
    }

    // move cursor back
    term_up(env->term, clear_rows - visible_cursor_row);
}

// Prompt prefix lines are emitted above the editor and are not included in eb->cur_rows.
// Clear both regions and leave the cursor at their shared origin.
static void edit_clear_with_prompt_prefix(ic_env_t* env, editor_t* eb,
                                          ssize_t prompt_prefix_lines) {
    if (env == NULL || eb == NULL) {
        return;
    }

    const ssize_t display_rows = (eb->view_rows > 0 ? eb->view_rows : 1);
    const ssize_t cursor_row = edit_view_cursor_row(eb);
    if (prompt_prefix_lines < 0) {
        prompt_prefix_lines = 0;
    }

    const ssize_t total_rows = prompt_prefix_lines + display_rows;
    term_attr_reset(env->term);
    term_start_of_line(env->term);
    term_up(env->term, prompt_prefix_lines + cursor_row);
    for (ssize_t i = 0; i < total_rows; ++i) {
        term_clear_line(env->term);
        term_writeln(env->term, "");
    }
    term_up(env->term, total_rows);
}

// Only called between editing operations, never from a completion/highlight callback.
// Keep the editor (including undo, input, hints and cursor) alive while moving its
// display below the notifications. Nested menus defer this until they return.
static void edit_flush_notifications(ic_env_t* env, editor_t* eb) {
    if (env->notifications != NULL && sbuf_len(env->notifications) > 0 &&
        !env->readline_terminal_suspended) {
        stringbuf_t* messages = env->notifications;
        env->notifications = NULL;
        edit_clear_with_prompt_prefix(env, eb, eb->prompt_prefix_lines);
        ic_term_abort_input_region(env);
        term_write(env->term, sbuf_string(messages));
        sbuf_free(messages);

        term_reset_line_state(env->term);
        term_start_of_line(env->term);
        eb->cur_row = 0;
        eb->cur_rows = 1;
        eb->input_rows = 1;
        eb->view_first_row = 0;
        eb->view_rows = 1;
        eb->view_input_rows = 1;
        eb->last_screen_cursor_known = false;
        redraw_prompt_prefix_lines(env, eb);
        edit_refresh(env, eb);
    }
}

static void edit_process_readline_event(ic_env_t* env) {
    if (env->readline_event_pending) {
        env->readline_event_pending = false;
        if (env->readline_event_callback != NULL) {
            env->readline_event_callback(env->readline_event_arg);
        }
    }
}

// clear screen and refresh
static void edit_clear_screen(ic_env_t* env, editor_t* eb) {
    const ssize_t view_rows = eb->view_rows;
    eb->view_rows = term_get_height(env->term) - 1;
    edit_clear(env, eb);
    eb->view_rows = view_rows;
    if (eb->prompt_prefix_lines > 0) {
        redraw_prompt_prefix_lines(env, eb);
    }
    edit_refresh(env, eb);
}

// refresh after a terminal window resized (but before doing further edit
// operations!)
static bool edit_resize(ic_env_t* env, editor_t* eb) {
    // update dimensions
    (void)term_update_dim(env->term);
    const ssize_t newtermw = term_get_width(env->term);
    const ssize_t newtermh = term_get_height(env->term);
    const bool width_changed = (eb->termw != newtermw);
    const bool height_changed = (eb->termh != newtermh);
    if (!width_changed && !height_changed) {
        return false;
    }

    eb->termh = newtermh;
    if (!width_changed) {
        if (eb->completion_auto_menu_visible) {
            edit_refresh_completion_auto_menu(env, eb);
        } else {
            edit_refresh(env, eb);
        }
        return true;
    }

    // recalculate the row layout assuming the hardwrapping for the new terminal
    // width
    ssize_t promptw, cpromptw;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);
    (void)sbuf_insert_at(eb->input, sbuf_string(eb->hint),
                         eb->pos);  // insert used hint

    // render extra (status lines, hint help, completion menu)
    stringbuf_t* extra = NULL;
    const bool menu_active = (sbuf_len(eb->extra) > 0);

    if (!menu_active && sbuf_len(eb->status) > 0) {
        extra = sbuf_new(eb->mem);
        if (extra != NULL) {
            bbcode_append(env->bbcode, sbuf_string(eb->status), extra, NULL);
        }
    }

    if (sbuf_len(eb->hint_help) > 0) {
        if (extra == NULL) {
            extra = sbuf_new(eb->mem);
        }
        if (extra != NULL) {
            if (sbuf_len(extra) > 0 && !sbuf_ends_with_newline(extra)) {
                bbcode_append(env->bbcode, "\n", extra, NULL);
            }
            bbcode_append(env->bbcode, sbuf_string(eb->hint_help), extra, NULL);
        }
    }

    if (menu_active) {
        if (extra == NULL) {
            extra = sbuf_new(eb->mem);
        }
        if (extra != NULL) {
            if (sbuf_len(extra) > 0 && !sbuf_ends_with_newline(extra)) {
                bbcode_append(env->bbcode, "\n", extra, NULL);
            }
            bbcode_append(env->bbcode, sbuf_string(eb->extra), extra, NULL);
        }
    }
    rowcol_t rc = {0};
    const ssize_t rows_input =
        sbuf_get_wrapped_rc_at_pos(eb->input, eb->termw, newtermw, promptw, cpromptw,
                                   env->line_wrap_marker_width, eb->pos, &rc);
    rowcol_t rc_extra = {0};
    ssize_t rows_extra = 0;
    if (extra != NULL) {
        rows_extra = sbuf_get_wrapped_rc_at_pos(extra, eb->termw, newtermw, 0, 0,
                                                env->line_wrap_marker_width, 0 /*pos*/, &rc_extra);
    }
    ssize_t rows = rows_input + rows_extra;
    debug_msg(
        "edit: resize: new rows: %zd, cursor row: %zd (previous: rows: %zd, "
        "cursor row %zd)\n",
        rows, rc.row, eb->cur_rows, eb->cur_row);

    // update the newly calculated row and rows (but keep enough history to clear old lines)
    if (rc.row > eb->cur_row) {
        eb->cur_row = rc.row;
    }
    if (rows > eb->cur_rows) {
        eb->cur_rows = rows;
    } else if (rows < eb->cur_rows) {
        eb->cur_rows++;
    }
    eb->termw = newtermw;
    if (eb->completion_auto_menu_visible) {
        edit_refresh_completion_auto_menu(env, eb);
    } else {
        edit_refresh(env, eb);
    }

    // remove hint again
    sbuf_delete_at(eb->input, eb->pos, sbuf_len(eb->hint));
    sbuf_free(extra);
    return true;
}

static void editor_append_hint_help(editor_t* eb, const char* help) {
    sbuf_clear(eb->hint_help);
    if (help != NULL) {
        sbuf_replace(eb->hint_help, "[ic-info]");
        (void)sbuf_append(eb->hint_help, help);
        (void)sbuf_append(eb->hint_help, "[/ic-info]\n");
    }
}

// refresh with possible hint
static void edit_refresh_hint(ic_env_t* env, editor_t* eb) {
    if (eb->refresh_suppressed) {
        eb->refresh_pending = true;
        return;
    }

    eb->refresh_pending = false;

    // Hints are recomputed on every edit; clear any previous inline state first
    // so stale suggestions are never redrawn when the new buffer has no hint.
    sbuf_clear(eb->hint);
    sbuf_clear(eb->hint_help);
    completions_clear(env->completions);

    if (eb->completion_auto_menu_visible) {
        sbuf_clear(eb->extra);
        eb->completion_auto_menu_visible = false;
    }
    if (env->completion_auto_menu) {
        // Shared search/palette menus temporarily disable undo while editing their query.
        // Neither they nor an active completion menu should spawn a passive menu.
        if (!eb->completion_menu_active && !eb->disable_undo) {
            edit_refresh_completion_auto_menu(env, eb);
        } else {
            edit_refresh(env, eb);
        }
        return;
    }

    if (env->no_hint || edit_current_line_is_empty(eb)) {
        edit_refresh(env, eb);
        return;
    }

    if (env->hint_delay > 0) {
        // refresh without hint first
        edit_refresh(env, eb);
    }

    // and see if we can construct a hint (displayed after a delay)
    ssize_t count =
        completions_generate_hint(env, env->completions, sbuf_string(eb->input), eb->pos, 2);
    bool has_spell_completion =
        (count >= 1 && edit_completion_is_current_word_spell(env, eb, 0, NULL, NULL));
    if (count >= 1) {
        const char* help = NULL;
        const char* hint = completions_get_hint(env->completions, 0, &help);
        if (hint != NULL && *hint != '\0') {
            sbuf_replace(eb->hint, hint);
            editor_append_hint_help(eb, help);
            // do auto-tabbing?
            if (env->complete_autotab) {
                stringbuf_t* sb = sbuf_new(env->mem);  // temporary buffer for completion
                if (sb != NULL) {
                    sbuf_replace(sb, sbuf_string(eb->input));
                    ssize_t pos = eb->pos;
                    const char* extra_hint = hint;
                    do {
                        ssize_t newpos = sbuf_insert_at(sb, extra_hint, pos);
                        if (newpos <= pos) {
                            break;
                        }
                        pos = newpos;
                        count = completions_generate_hint(env, env->completions, sbuf_string(sb),
                                                          pos, 2);
                        if (count == 1) {
                            const char* extra_help = NULL;
                            extra_hint = completions_get_hint(env->completions, 0, &extra_help);
                            if (extra_hint != NULL) {
                                editor_append_hint_help(eb, extra_help);
                                (void)sbuf_append(eb->hint, extra_hint);
                            }
                        }
                    } while (count == 1);
                    sbuf_free(sb);
                }
            }
        }
    }

    if (env->hint_delay <= 0 || has_spell_completion) {
        // refresh with hint directly
        edit_refresh(env, eb);
    }
}

//-------------------------------------------------------------
// Edit operations
//-------------------------------------------------------------

static void edit_undo_restore(ic_env_t* env, editor_t* eb) {
    editor_undo_restore(eb, true);
    if (env->completion_auto_menu) {
        edit_refresh_hint(env, eb);
    } else {
        edit_refresh(env, eb);
    }
}

static void edit_redo_restore(ic_env_t* env, editor_t* eb) {
    editor_redo_restore(eb);
    if (env->completion_auto_menu) {
        edit_refresh_hint(env, eb);
    } else {
        edit_refresh(env, eb);
    }
}

static void edit_cursor_left(ic_env_t* env, editor_t* eb) {
    ssize_t cwidth = 1;
    ssize_t prev = sbuf_prev(eb->input, eb->pos, &cwidth);
    if (prev < 0) {
        return;
    }
    rowcol_t rc;
    (void)edit_get_rowcol(env, eb, &rc);
    eb->pos = prev;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_right(ic_env_t* env, editor_t* eb) {
    ssize_t cwidth = 1;
    ssize_t next = sbuf_next(eb->input, eb->pos, &cwidth);
    if (next < 0) {
        return;
    }
    rowcol_t rc;
    (void)edit_get_rowcol(env, eb, &rc);
    eb->pos = next;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_line_end(ic_env_t* env, editor_t* eb) {
    ssize_t end = sbuf_find_line_end(eb->input, eb->pos);
    if (end < 0) {
        return;
    }
    eb->pos = end;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_ctrl_e(ic_env_t* env, editor_t* eb) {
    edit_cursor_line_end(env, eb);
}

static void edit_cursor_ctrl_a(ic_env_t* env, editor_t* eb) {
    edit_cursor_line_start(env, eb);
}

static void edit_cursor_line_start(ic_env_t* env, editor_t* eb) {
    ssize_t start = sbuf_find_line_start(eb->input, eb->pos);
    if (start < 0) {
        return;
    }
    eb->pos = start;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_next_word(ic_env_t* env, editor_t* eb) {
    ssize_t end = sbuf_find_word_end(eb->input, eb->pos);
    if (end < 0) {
        return;
    }
    eb->pos = end;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_prev_word(ic_env_t* env, editor_t* eb) {
    ssize_t start = sbuf_find_word_start(eb->input, eb->pos);
    if (start < 0) {
        return;
    }
    eb->pos = start;
    edit_refresh_hint(env, eb);
}

static ic_maybe_unused void edit_cursor_next_ws_word(ic_env_t* env, editor_t* eb) {
    ssize_t end = sbuf_find_ws_word_end(eb->input, eb->pos);
    if (end < 0) {
        return;
    }
    eb->pos = end;
    edit_refresh_hint(env, eb);
}

static ic_maybe_unused void edit_cursor_prev_ws_word(ic_env_t* env, editor_t* eb) {
    ssize_t start = sbuf_find_ws_word_start(eb->input, eb->pos);
    if (start < 0) {
        return;
    }
    eb->pos = start;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_to_start(ic_env_t* env, editor_t* eb) {
    eb->pos = 0;
    edit_refresh_hint(env, eb);
}

static void edit_cursor_to_end(ic_env_t* env, editor_t* eb) {
    eb->pos = sbuf_len(eb->input);
    edit_refresh_hint(env, eb);
}

static void edit_cursor_row_up(ic_env_t* env, editor_t* eb) {
    rowcol_t rc;
    (void)edit_get_rowcol(env, eb, &rc);
    // Recalled entries end here so repeated Up presses can skip multiline content.
    if (editor_pos_is_at_end(eb) || rc.row == 0) {
        edit_history_prev(env, eb);
    } else {
        edit_set_pos_at_rowcol(env, eb, rc.row - 1, rc.col);
    }
}

static void edit_history_prev_with_spell(ic_env_t* env, editor_t* eb) {
    ssize_t previous_history_idx = eb->history_idx;
    char* previous_input = mem_strdup(env->mem, sbuf_string(eb->input));
    edit_history_prev(env, eb);

    bool history_changed = (eb->history_idx != previous_history_idx);
    if (!history_changed && previous_input != NULL) {
        const char* current_input = sbuf_string(eb->input);
        history_changed = (current_input == NULL || strcmp(previous_input, current_input) != 0);
    }
    if (previous_input != NULL) {
        mem_free(env->mem, previous_input);
    }
    if (!history_changed || !env->spell_correct) {
        return;
    }

    eb->pos = sbuf_len(eb->input);
    const char* input = sbuf_string(eb->input);
    if (input == NULL || eb->pos <= 0) {
        completions_clear(env->completions);
        return;
    }

    ssize_t prev = str_prev_ofs(input, eb->pos, NULL);
    if (prev <= 0 || ic_char_is_separator(input + eb->pos - prev, (long)prev)) {
        completions_clear(env->completions);
        return;
    }

    ssize_t word_start = edit_find_word_start(input, eb->pos);
    if (word_start < 0 || word_start >= eb->pos) {
        completions_clear(env->completions);
        return;
    }

    ssize_t word_len = eb->pos - word_start;
    char* original_word = mem_strndup(env->mem, input + word_start, word_len);
    if (original_word == NULL) {
        completions_clear(env->completions);
        return;
    }

    ssize_t count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos,
                                         IC_MAX_COMPLETIONS_TO_TRY);
    if (count <= 0) {
        completions_clear(env->completions);
        mem_free(env->mem, original_word);
        return;
    }

    if (completions_all_sources_equal(env->completions, "spell")) {
        (void)edit_complete(env, eb, 0);
        mem_free(env->mem, original_word);
        completions_clear(env->completions);
        return;
    }

    const ssize_t best_spell_idx = edit_best_spell_candidate(env, original_word, count, "spell");
    if (best_spell_idx >= 0) {
        (void)edit_complete(env, eb, best_spell_idx);
    }

    mem_free(env->mem, original_word);
    completions_clear(env->completions);
}

static void edit_cursor_row_down(ic_env_t* env, editor_t* eb) {
    rowcol_t rc;
    ssize_t rows = edit_get_rowcol(env, eb, &rc);
    if (editor_pos_is_at_end(eb) || rc.row + 1 >= rows) {
        edit_history_next(env, eb);
    } else {
        edit_set_pos_at_rowcol(env, eb, rc.row + 1, rc.col);
    }
}

static void edit_cursor_match_brace(ic_env_t* env, editor_t* eb) {
    ssize_t match =
        find_matching_brace(sbuf_string(eb->input), eb->pos, ic_env_get_match_braces(env), NULL);
    if (match < 0) {
        return;
    }
    eb->pos = match;
    edit_refresh_hint(env, eb);
}

static void edit_backspace(ic_env_t* env, editor_t* eb) {
    if (eb->pos <= 0) {
        return;
    }
    editor_start_modify(eb);
    eb->pos = sbuf_delete_char_before(eb->input, eb->pos);
    edit_refresh_hint(env, eb);
}

static void edit_delete_char(ic_env_t* env, editor_t* eb) {
    if (eb->pos >= sbuf_len(eb->input)) {
        return;
    }
    editor_start_modify(eb);
    sbuf_delete_char_at(eb->input, eb->pos);
    edit_refresh_hint(env, eb);
}

static void edit_delete_all(ic_env_t* env, editor_t* eb) {
    if (sbuf_len(eb->input) <= 0) {
        return;
    }
    editor_start_modify(eb);
    sbuf_clear(eb->input);
    eb->pos = 0;
    edit_refresh(env, eb);
}

static bool edit_get_line_bounds(editor_t* eb, ssize_t* start, ssize_t* end) {
    if (eb == NULL || start == NULL || end == NULL) {
        return false;
    }
    *start = sbuf_find_line_start(eb->input, eb->pos);
    if (*start < 0) {
        return false;
    }
    *end = sbuf_find_line_end(eb->input, eb->pos);
    if (*end < 0) {
        return false;
    }
    return true;
}

typedef ssize_t (*edit_boundary_finder_t)(stringbuf_t*, ssize_t);

static void edit_delete_to_boundary(ic_env_t* env, editor_t* eb, edit_boundary_finder_t finder,
                                    bool delete_to_start) {
    if (finder == NULL) {
        return;
    }
    ssize_t boundary = finder(eb->input, eb->pos);
    if (boundary < 0) {
        return;
    }
    editor_start_modify(eb);
    if (delete_to_start) {
        sbuf_delete_from_to(eb->input, boundary, eb->pos);
        eb->pos = boundary;
    } else {
        sbuf_delete_from_to(eb->input, eb->pos, boundary);
    }
    edit_refresh(env, eb);
}

static void edit_delete_to_end_of_line(ic_env_t* env, editor_t* eb) {
    ssize_t start = 0;
    ssize_t end = 0;
    if (!edit_get_line_bounds(eb, &start, &end)) {
        return;
    }
    editor_start_modify(eb);
    // if on an empty line, remove it completely
    if (start == end && sbuf_char_at(eb->input, end) == '\n') {
        end++;
    } else if (start == end && sbuf_char_at(eb->input, start - 1) == '\n') {
        eb->pos--;
    }
    sbuf_delete_from_to(eb->input, eb->pos, end);
    edit_refresh(env, eb);
}

static void edit_delete_to_start_of_line(ic_env_t* env, editor_t* eb) {
    ssize_t start = 0;
    ssize_t end = 0;
    if (!edit_get_line_bounds(eb, &start, &end)) {
        return;
    }
    editor_start_modify(eb);
    // delete start newline if it was an empty line
    bool goright = false;
    if (start > 0 && sbuf_char_at(eb->input, start - 1) == '\n' && start == end) {
        // if it is an empty line remove it
        start--;
        // afterwards, move to start of next line if it exists (so the cursor
        // stays on the same row)
        goright = true;
    }
    sbuf_delete_from_to(eb->input, start, eb->pos);
    eb->pos = start;
    if (goright) {
        edit_cursor_right(env, eb);
    }
    edit_refresh(env, eb);
}

static ic_maybe_unused void edit_delete_line(ic_env_t* env, editor_t* eb) {
    ssize_t start = 0;
    ssize_t end = 0;
    if (!edit_get_line_bounds(eb, &start, &end)) {
        return;
    }
    editor_start_modify(eb);
    // delete newline as well so no empty line is left;
    bool goright = false;
    if (start > 0 && sbuf_char_at(eb->input, start - 1) == '\n') {
        start--;
        // afterwards, move to start of next line if it exists (so the cursor
        // stays on the same row)
        goright = true;
    } else if (sbuf_char_at(eb->input, end) == '\n') {
        end++;
    }
    sbuf_delete_from_to(eb->input, start, end);
    eb->pos = start;
    if (goright) {
        edit_cursor_right(env, eb);
    }
    edit_refresh(env, eb);
}

static void edit_delete_to_start_of_word(ic_env_t* env, editor_t* eb) {
    edit_delete_to_boundary(env, eb, sbuf_find_word_start, true);
}

static void edit_delete_to_end_of_word(ic_env_t* env, editor_t* eb) {
    edit_delete_to_boundary(env, eb, sbuf_find_word_end, false);
}

static void edit_delete_to_start_of_ws_word(ic_env_t* env, editor_t* eb) {
    edit_delete_to_boundary(env, eb, sbuf_find_ws_word_start, true);
}

static ic_maybe_unused void edit_delete_to_end_of_ws_word(ic_env_t* env, editor_t* eb) {
    edit_delete_to_boundary(env, eb, sbuf_find_ws_word_end, false);
}

static ic_maybe_unused void edit_delete_word(ic_env_t* env, editor_t* eb) {
    ssize_t start = sbuf_find_word_start(eb->input, eb->pos);
    if (start < 0) {
        return;
    }
    ssize_t end = sbuf_find_word_end(eb->input, eb->pos);
    if (end < 0) {
        return;
    }
    editor_start_modify(eb);
    sbuf_delete_from_to(eb->input, start, end);
    eb->pos = start;
    edit_refresh(env, eb);
}

static void edit_swap_char(ic_env_t* env, editor_t* eb) {
    if (eb->pos <= 0 || eb->pos == sbuf_len(eb->input)) {
        return;
    }
    editor_start_modify(eb);
    eb->pos = sbuf_swap_char(eb->input, eb->pos);
    edit_refresh(env, eb);
}

static void edit_multiline_eol(ic_env_t* env, editor_t* eb) {
    if (eb->pos <= 0) {
        return;
    }
    if (sbuf_string(eb->input)[eb->pos - 1] != env->multiline_eol) {
        return;
    }
    editor_start_modify(eb);
    if (env->retain_multiline_continuation) {
        ssize_t nextpos = sbuf_insert_at(eb->input, "\n", eb->pos);
        if (nextpos >= 0) {
            eb->pos = nextpos;
        }
        edit_refresh(env, eb);
        return;
    }
    // replace line continuation with a real newline
    sbuf_delete_at(eb->input, eb->pos - 1, 1);
    (void)sbuf_insert_at(eb->input, "\n", eb->pos - 1);
    edit_refresh(env, eb);
}

static void edit_insert_unicode(ic_env_t* env, editor_t* eb, unicode_t u) {
    editor_start_modify(eb);
    ssize_t nextpos = sbuf_insert_unicode_at(eb->input, u, eb->pos);
    if (nextpos >= 0) {
        eb->pos = nextpos;
    }
    edit_refresh_hint(env, eb);
}

// The continuation callback determines whether a buffer is incomplete.  The
// editor can then give its newly created line a useful shell-style indent
// without needing to understand the caller's full grammar.
static bool edit_line_opens_indented_block(const char* input, ssize_t line_start,
                                           ssize_t line_end) {
    while (line_end > line_start && (input[line_end - 1] == ' ' || input[line_end - 1] == '\t')) {
        --line_end;
    }
    if (line_end <= line_start) {
        return false;
    }

    const char last = input[line_end - 1];
    if (last == '{' || last == '(' || last == '[') {
        return true;
    }

    ssize_t word_start = line_end;
    while (word_start > line_start && isalpha((unsigned char)input[word_start - 1])) {
        --word_start;
    }
    const ssize_t word_len = line_end - word_start;
    return (word_len == 2 && strncmp(input + word_start, "do", 2) == 0) ||
           (word_len == 4 && strncmp(input + word_start, "then", 4) == 0) ||
           (word_len == 2 && strncmp(input + word_start, "in", 2) == 0);
}

static void edit_insert_auto_indented_linefeed(ic_env_t* env, editor_t* eb) {
    assert(env != NULL && eb != NULL);

    const ssize_t line_end = eb->pos;
    const char* input = sbuf_string(eb->input);
    ssize_t line_start = line_end;
    while (line_start > 0 && input[line_start - 1] != '\n') {
        --line_start;
    }
    ssize_t inherited_indent = line_start;
    while (inherited_indent < line_end &&
           (input[inherited_indent] == ' ' || input[inherited_indent] == '\t')) {
        ++inherited_indent;
    }
    inherited_indent -= line_start;
    const bool add_indent_level =
        !env->no_multiline_indent && edit_line_opens_indented_block(input, line_start, line_end);

    editor_start_modify(eb);
    ssize_t nextpos = sbuf_insert_char_at(eb->input, '\n', eb->pos);
    if (nextpos < 0) {
        return;
    }
    eb->pos = nextpos;

    if (!env->no_multiline_indent) {
        for (ssize_t i = 0; i < inherited_indent; ++i) {
            const char whitespace = sbuf_char_at(eb->input, line_start + i);
            nextpos = sbuf_insert_char_at(eb->input, whitespace, eb->pos);
            if (nextpos < 0) {
                break;
            }
            eb->pos = nextpos;
        }
        if (add_indent_level) {
            nextpos = sbuf_insert_at(eb->input, "  ", eb->pos);
            if (nextpos >= 0) {
                eb->pos = nextpos;
            }
        }
    }
    edit_refresh(env, eb);
}

static bool edit_is_word_char(char ch) {
    return (ch != 0 && (isalnum((unsigned char)ch) || ch == '_'));
}

static bool edit_is_escaped_at(stringbuf_t* input, ssize_t index) {
    if (input == NULL || index <= 0) {
        return false;
    }
    ssize_t backslash_count = 0;
    for (ssize_t i = index - 1; i >= 0; --i) {
        if (sbuf_char_at(input, i) != '\\') {
            break;
        }
        backslash_count++;
    }
    return (backslash_count % 2) == 1;
}

static void edit_auto_brace(ic_env_t* env, editor_t* eb, char c) {
    if (env->no_autobrace) {
        return;
    }
    const char* braces = ic_env_get_auto_braces(env);
    for (const char* b = braces; *b != 0; b += 2) {
        const char open = b[0];
        const char close = b[1];
        const bool symmetric = (open == close);
        if (open == c) {
            if (symmetric) {
                const ssize_t inserted_index = eb->pos - 1;
                const bool escaped = edit_is_escaped_at(eb->input, inserted_index);
                const char next = sbuf_char_at(eb->input, eb->pos);
                if (!escaped && next == close) {
                    // move over existing auto-inserted closing quote
                    sbuf_delete_char_at(eb->input, eb->pos);
                    return;
                }
                if (escaped) {
                    return;
                }
                if (open == '\'' &&
                    edit_is_word_char(sbuf_char_at(eb->input, inserted_index - 1))) {
                    return;
                }
                (void)sbuf_insert_char_at(eb->input, close, eb->pos);
                return;
            }
            (void)sbuf_insert_char_at(eb->input, close, eb->pos);
            bool balanced = false;
            (void)find_matching_brace(sbuf_string(eb->input), eb->pos, braces, &balanced);
            if (!balanced) {
                // don't insert if it leads to an unbalanced expression.
                sbuf_delete_char_at(eb->input, eb->pos);
            }
            return;
        } else if (!symmetric && close == c) {
            // close brace, check if we don't overwrite to the right
            if (sbuf_char_at(eb->input, eb->pos) == c) {
                sbuf_delete_char_at(eb->input, eb->pos);
            }
            return;
        }
    }
}

static void editor_auto_indent(editor_t* eb, const char* pre, const char* post) {
    assert(eb->pos > 0 && sbuf_char_at(eb->input, eb->pos - 1) == '\n');
    ssize_t prelen = ic_strlen(pre);
    if (prelen > 0) {
        if (eb->pos - 1 < prelen) {
            return;
        }
        if (!ic_starts_with(sbuf_string(eb->input) + eb->pos - 1 - prelen, pre)) {
            return;
        }
        if (!ic_starts_with(sbuf_string(eb->input) + eb->pos, post)) {
            return;
        }
        eb->pos = sbuf_insert_at(eb->input, "  ", eb->pos);
        (void)sbuf_insert_char_at(eb->input, '\n', eb->pos);
    }
}

static bool edit_expand_abbreviation_for_range(ic_env_t* env, editor_t* eb, const char* buffer,
                                               ssize_t word_start, ssize_t word_end,
                                               ssize_t cursor_delta, bool modification_started) {
    ssize_t word_len = word_end - word_start;
    if (word_len <= 0) {
        return false;
    }

    for (ssize_t i = 0; i < env->abbreviation_count; ++i) {
        ic_abbreviation_entry_t* entry = &env->abbreviations[i];
        if (entry->trigger_len == word_len &&
            strncmp(entry->trigger, buffer + word_start, (size_t)word_len) == 0) {
            if (!modification_started) {
                editor_start_modify(eb);
            }
            sbuf_delete_at(eb->input, word_start, word_len);
            if (cursor_delta < 0) {
                cursor_delta = 0;
            }
            eb->pos = word_start + cursor_delta;
            ssize_t new_pos = sbuf_insert_at(eb->input, entry->expansion, word_start);
            ssize_t expansion_len = new_pos - word_start;
            eb->pos += expansion_len;
            return true;
        }
    }

    return false;
}

static bool edit_try_expand_abbreviation(ic_env_t* env, editor_t* eb, bool boundary_char_present,
                                         bool modification_started) {
    if (env == NULL || eb == NULL) {
        return false;
    }
    if (env->abbreviation_count <= 0 || env->abbreviations == NULL) {
        return false;
    }

    const char* buffer = sbuf_string(eb->input);
    if (buffer == NULL) {
        return false;
    }

    ssize_t boundary_offset = (boundary_char_present ? 1 : 0);
    if (boundary_char_present && eb->pos <= boundary_offset) {
        return false;
    }

    if (eb->pos > boundary_offset) {
        if (boundary_char_present) {
            ssize_t boundary_index = eb->pos - 1;
            if (boundary_index < 0) {
                return false;
            }
            if (!ic_char_is_white(buffer + boundary_index, 1)) {
                return false;
            }
        }

        ssize_t word_end = eb->pos - boundary_offset;
        if (word_end > 0 && !ic_char_is_white(buffer + word_end - 1, 1)) {
            ssize_t word_start = sbuf_find_ws_word_start(eb->input, word_end);
            if (word_start < 0) {
                word_start = 0;
            }

            if (word_start == 0 || ic_char_is_white(buffer + word_start - 1, 1)) {
                ssize_t cursor_delta = eb->pos - word_end;
                if (edit_expand_abbreviation_for_range(env, eb, buffer, word_start, word_end,
                                                       cursor_delta, modification_started)) {
                    return true;
                }
            } else if (boundary_char_present) {
                return false;
            }
        } else if (boundary_char_present) {
            return false;
        }
    }

    if (!boundary_char_present) {
        ssize_t len = sbuf_len(eb->input);
        if ((eb->pos < len && !ic_char_is_white(buffer + eb->pos, 1)) &&
            (eb->pos == 0 || ic_char_is_white(buffer + eb->pos - 1, 1))) {
            ssize_t word_end = sbuf_find_ws_word_end(eb->input, eb->pos);
            if ((word_end > eb->pos) &&
                edit_expand_abbreviation_for_range(env, eb, buffer, eb->pos, word_end, 0,
                                                   modification_started)) {
                return true;
            }
        }
    }

    return false;
}

static bool edit_expand_abbreviation_if_needed(ic_env_t* env, editor_t* eb,
                                               bool modification_started) {
    if (env == NULL || eb == NULL || eb->input == NULL) {
        return false;
    }
    if (eb->pos <= 0) {
        return false;
    }

    const char* buffer = sbuf_string(eb->input);
    if (buffer == NULL) {
        return false;
    }

    if (ic_char_is_white(buffer + eb->pos - 1, 1) &&
        edit_try_expand_abbreviation(env, eb, true, modification_started)) {
        return true;
    }

    return edit_try_expand_abbreviation(env, eb, false, modification_started);
}

static void edit_insert_char(ic_env_t* env, editor_t* eb, char c) {
    editor_start_modify(eb);
    ssize_t nextpos = sbuf_insert_char_at(eb->input, c, eb->pos);
    if (nextpos >= 0) {
        eb->pos = nextpos;
    }
    if (c == ' ' || c == '\n' || c == '\r') {
        (void)edit_try_expand_abbreviation(env, eb, true, true);
    }
    edit_auto_brace(env, eb, c);
    if (c == '\n') {
        editor_auto_indent(eb, "{", "}");  // todo: custom auto indent tokens?
    }
    edit_refresh_hint(env, eb);
}

//-------------------------------------------------------------
// Help
//-------------------------------------------------------------

#include "editline_help.c"

//-------------------------------------------------------------
// Status hints
//-------------------------------------------------------------

enum {
    EDIT_STATUS_HINT_BUFFER_LEN = 512,
    EDIT_STATUS_HINT_KEYS_LEN = 128
};

static char* edit_escape_status_fragment(alloc_t* mem, const char* text) {
    if (mem == NULL || text == NULL) {
        return NULL;
    }

    const ssize_t len = ic_strlen(text);
    if (len < 0) {
        return NULL;
    }

    ssize_t extra = 0;
    for (ssize_t i = 0; i < len; ++i) {
        const char ch = text[i];
        if (ch == '[' || ch == '\\') {
            extra++;
        }
    }

    char* escaped = mem_malloc_tp_n(mem, char, (size_t)len + (size_t)extra + 1);
    if (escaped == NULL) {
        return NULL;
    }

    char* dest = escaped;
    for (ssize_t i = 0; i < len; ++i) {
        unsigned char uch = (unsigned char)text[i];
        char ch = ((uch < 0x20 || uch == 0x7F) ? ' ' : (char)uch);
        if (ch == '[' || ch == '\\') {
            *dest++ = '\\';
        }
        *dest++ = ch;
    }
    *dest = '\0';
    return escaped;
}

static bool edit_format_spell_status_hint(ic_env_t* env, editor_t* eb, char* buffer,
                                          size_t buflen) {
    if (env == NULL || eb == NULL || buffer == NULL || buflen == 0 || eb->input == NULL ||
        env->completions == NULL) {
        return false;
    }

    buffer[0] = '\0';

    if (eb->pos <= 0 || completions_count(env->completions) <= 0) {
        return false;
    }

    const char* input = sbuf_string(eb->input);
    if (input == NULL) {
        return false;
    }

    const char* replacement = NULL;
    ssize_t replacement_start = 0;
    if (!edit_completion_is_current_word_spell(env, eb, 0, &replacement, &replacement_start)) {
        return false;
    }

    char* original = mem_strndup(env->mem, input + replacement_start, eb->pos - replacement_start);
    if (original == NULL) {
        return false;
    }

    if (strcmp(original, replacement) == 0) {
        mem_free(env->mem, original);
        return false;
    }

    char* escaped_original = edit_escape_status_fragment(env->mem, original);
    char* escaped_replacement = edit_escape_status_fragment(env->mem, replacement);
    mem_free(env->mem, original);

    if (escaped_original == NULL || escaped_replacement == NULL) {
        mem_free(env->mem, escaped_original);
        mem_free(env->mem, escaped_replacement);
        return false;
    }

    char completion_keys[EDIT_STATUS_HINT_KEYS_LEN];
    format_binding_keys(env, IC_KEY_ACTION_COMPLETE, NULL, completion_keys, sizeof(completion_keys),
                        true);

    int written = snprintf(buffer, buflen, "[ic-status]spell: %s -> %s  accept: %s[/]",
                           escaped_original, escaped_replacement, completion_keys);

    mem_free(env->mem, escaped_original);
    mem_free(env->mem, escaped_replacement);

    if (written < 0) {
        buffer[0] = '\0';
        return false;
    }

    return true;
}

static bool edit_format_default_status_hints(ic_env_t* env, char* buffer, size_t buflen) {
    if (env == NULL || buffer == NULL || buflen == 0) {
        return false;
    }

    char completion_keys[EDIT_STATUS_HINT_KEYS_LEN];
    char history_search_keys[EDIT_STATUS_HINT_KEYS_LEN];
    char command_palette_keys[EDIT_STATUS_HINT_KEYS_LEN];
    char help_keys[EDIT_STATUS_HINT_KEYS_LEN];

    format_binding_keys(env, IC_KEY_ACTION_COMPLETE, NULL, completion_keys, sizeof(completion_keys),
                        true);
    format_binding_keys(env, IC_KEY_ACTION_HISTORY_SEARCH, NULL, history_search_keys,
                        sizeof(history_search_keys), true);
    format_binding_keys(env, IC_KEY_ACTION_COMMAND_PALETTE, NULL, command_palette_keys,
                        sizeof(command_palette_keys), true);
    format_binding_keys(env, IC_KEY_ACTION_SHOW_HELP, NULL, help_keys, sizeof(help_keys), true);

    int written = snprintf(buffer, buflen, "[ic-status]complete: %s  search: %s  help: %s[/]",
                           completion_keys, history_search_keys, help_keys);
    if (written < 0) {
        buffer[0] = '\0';
        return false;
    }
    return true;
}

static void edit_format_mouse_enabled_status_hint(ic_env_t* env, bool include_disable_hint,
                                                  char* buffer, size_t buflen) {
    if (buffer == NULL || buflen == 0) {
        return;
    }

    if (include_disable_hint) {
        char mouse_toggle_keys[EDIT_STATUS_HINT_KEYS_LEN];
        format_binding_keys(env, IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING, NULL, mouse_toggle_keys,
                            sizeof(mouse_toggle_keys), true);
        if (mouse_toggle_keys[0] != '\0' && strcmp(mouse_toggle_keys, "(unbound)") != 0 &&
            snprintf(buffer, buflen, "Mouse clicking is enabled (press %s to disable)",
                     mouse_toggle_keys) >= 0) {
            return;
        }
    }

    (void)ic_strncpy(buffer, (ssize_t)buflen, "Mouse clicking is enabled", (ssize_t)buflen - 1);
}

static ic_mouse_clicking_mode_t edit_normalize_mouse_mode(ic_mouse_clicking_mode_t mode) {
    switch (mode) {
        case IC_MOUSE_CLICKING_DISABLED:
        case IC_MOUSE_CLICKING_SIMPLE:
        case IC_MOUSE_CLICKING_SMART:
        case IC_MOUSE_CLICKING_MENU_ONLY:
            return mode;
        default:
            return IC_MOUSE_CLICKING_SMART;
    }
}

static bool edit_mouse_mode_supports_editing_capture(ic_mouse_clicking_mode_t mode) {
    const ic_mouse_clicking_mode_t normalized = edit_normalize_mouse_mode(mode);
    return (normalized == IC_MOUSE_CLICKING_SIMPLE || normalized == IC_MOUSE_CLICKING_SMART);
}

static bool edit_mouse_mode_is_menu_only(ic_mouse_clicking_mode_t mode) {
    return (edit_normalize_mouse_mode(mode) == IC_MOUSE_CLICKING_MENU_ONLY);
}

static bool edit_mouse_capture_should_be_enabled(const editor_t* eb) {
    if (eb == NULL) {
        return false;
    }

    if (!edit_mouse_mode_supports_editing_capture(eb->mouse_reporting_mode) ||
        !eb->mouse_reporting_manual_enabled) {
        return false;
    }

    if (eb->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART && eb->mouse_reporting_auto_suspended) {
        return false;
    }

    return true;
}

static void edit_set_mouse_focus_reporting(ic_env_t* env, editor_t* eb, bool enabled) {
    if (eb == NULL) {
        return;
    }

    if (eb->mouse_focus_reporting_enabled == enabled) {
        return;
    }

    if (env == NULL || env->term == NULL || !term_is_interactive(env->term)) {
        eb->mouse_focus_reporting_enabled = false;
        return;
    }

    term_write(env->term, (enabled ? "\x1b[?1004h" : "\x1b[?1004l"));
    term_flush(env->term);
    eb->mouse_focus_reporting_enabled = enabled;
}

static bool edit_enable_mouse_tracking(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || env->term == NULL || !term_is_interactive(env->term)) {
        return false;
    }

    if (eb->mouse_capture_depth == 0) {
        term_write(env->term, "\x1b[?1000h\x1b[?1006h");
        if (eb->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART) {
            term_write(env->term, "\x1b[?1002h");
        }
        term_flush(env->term);
    }

    if (eb->mouse_capture_depth < SSIZE_MAX) {
        eb->mouse_capture_depth++;
    }
    return true;
}

static void edit_force_mouse_tracking_disabled(ic_env_t* env, editor_t* eb) {
    if (eb == NULL) {
        return;
    }

    eb->mouse_capture_depth = 0;
    eb->mouse_left_button_down = false;

    if (env == NULL || env->term == NULL || !term_is_interactive(env->term)) {
        return;
    }

    term_write(env->term, "\x1b[?1002l\x1b[?1000l\x1b[?1006l");
    term_flush(env->term);
}

static void edit_disable_mouse_tracking(ic_env_t* env, editor_t* eb, bool enabled) {
    if (!enabled || env == NULL || eb == NULL || env->term == NULL ||
        !term_is_interactive(env->term)) {
        return;
    }

    if (eb->mouse_capture_depth > 0) {
        eb->mouse_capture_depth--;
    }

    if (eb->mouse_capture_depth == 0) {
        eb->mouse_left_button_down = false;
        term_write(env->term, "\x1b[?1002l\x1b[?1000l\x1b[?1006l");
        term_flush(env->term);
    }
}

static void edit_set_mouse_reporting_enabled(ic_env_t* env, editor_t* eb, bool enabled) {
    if (eb == NULL) {
        return;
    }

    if (enabled) {
        if (eb->mouse_reporting_enabled) {
            return;
        }
        if (edit_enable_mouse_tracking(env, eb)) {
            eb->mouse_reporting_enabled = true;
        }
        return;
    }

    eb->mouse_reporting_enabled = false;
    edit_force_mouse_tracking_disabled(env, eb);
}

static void edit_apply_mouse_reporting_policy(ic_env_t* env, editor_t* eb) {
    if (eb == NULL) {
        return;
    }

    edit_set_mouse_reporting_enabled(env, eb, edit_mouse_capture_should_be_enabled(eb));
}

static void edit_set_mouse_manual_enabled(ic_env_t* env, editor_t* eb, bool enabled) {
    if (eb == NULL) {
        return;
    }

    if (!edit_mouse_mode_supports_editing_capture(eb->mouse_reporting_mode)) {
        enabled = false;
    }

    eb->mouse_reporting_manual_enabled = enabled;
    if (!enabled) {
        eb->mouse_reporting_auto_suspended = false;
        eb->mouse_terminal_selection_suspended = false;
    }

    edit_apply_mouse_reporting_policy(env, eb);
}

static void edit_set_mouse_auto_suspended(ic_env_t* env, editor_t* eb, bool suspended,
                                          bool terminal_selection) {
    if (eb == NULL) {
        return;
    }

    if (eb->mouse_reporting_mode != IC_MOUSE_CLICKING_SMART ||
        !eb->mouse_reporting_manual_enabled) {
        suspended = false;
        terminal_selection = false;
    }

    // Capture can resume on button release while repainting stays paused, so the
    // native selection remains visible until the next click or keyboard input.
    const bool selection_suspended = terminal_selection;
    if (eb->mouse_reporting_auto_suspended == suspended &&
        eb->mouse_terminal_selection_suspended == selection_suspended) {
        return;
    }

    eb->mouse_reporting_auto_suspended = suspended;
    eb->mouse_terminal_selection_suspended = selection_suspended;
    edit_apply_mouse_reporting_policy(env, eb);
}

static bool edit_binding_specs_include_key(const char* specs, code_t key) {
    if (specs == NULL || specs[0] == '\0') {
        return false;
    }

    const char* p = specs;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '|') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char* start = p;
        while (*p != '\0' && *p != '|') {
            p++;
        }
        const char* end = p;

        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        while (start < end && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if (start >= end) {
            continue;
        }

        size_t len = (size_t)(end - start);
        if (len >= 64) {
            continue;
        }

        char token[64];
        memcpy(token, start, len);
        token[len] = '\0';

        ic_keycode_t parsed_key = IC_KEY_NONE;
        if (ic_parse_key_spec(token, &parsed_key) && parsed_key == key) {
            return true;
        }
    }

    return false;
}

static bool edit_key_is_action_binding(const ic_env_t* env, code_t key, ic_key_action_t action) {
    if (env == NULL) {
        return false;
    }

    ic_key_action_t bound_action = IC_KEY_ACTION__MAX;
    if (key_binding_lookup_action(env, key, &bound_action)) {
        return (bound_action == action);
    }

    const char* specs = ic_key_binding_profile_default_specs(action);
    return edit_binding_specs_include_key(specs, key);
}

static bool edit_key_is_mouse_toggle_binding(const ic_env_t* env, code_t key) {
    return edit_key_is_action_binding(env, key, IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING);
}

static bool edit_mouse_auto_resume_triggered_by_key(code_t key) {
    code_t key_no_mods = KEY_NO_MODS(key);
    if (key_no_mods == KEY_NONE) {
        return false;
    }

    if (key_no_mods == KEY_EVENT_FOCUS_IN) {
        return true;
    }

    return (key_no_mods < KEY_EVENT_BASE);
}

// Returns true when a selection event was consumed and must not position the
// editing cursor or accept an item in a menu.
static bool edit_maybe_resume_smart_mouse_reporting(ic_env_t* env, editor_t* eb, code_t key) {
    if (env == NULL || eb == NULL) {
        return false;
    }

    const code_t key_no_mods = KEY_NO_MODS(key);
    if (key_no_mods != KEY_EVENT_MOUSE_OTHER && key_no_mods != KEY_NONE) {
        eb->mouse_left_button_down = false;
    }

    if (eb->mouse_reporting_mode != IC_MOUSE_CLICKING_SMART ||
        !eb->mouse_reporting_manual_enabled) {
        return false;
    }

    if (eb->mouse_terminal_selection_suspended && key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        tty_mouse_event_t event;
        if (tty_get_last_mouse_event(env->tty, &event)) {
            if (eb->mouse_reporting_auto_suspended &&
                event.action == TTY_MOUSE_ACTION_LEFT_RELEASE) {
                // Not all terminals deliver a release after tracking is disabled.
                // When one does, restore capture without repainting the selection.
                edit_set_mouse_auto_suspended(env, eb, false, true);
                return true;
            }
            if (!eb->mouse_reporting_auto_suspended) {
                if (event.action == TTY_MOUSE_ACTION_LEFT_PRESS) {
                    edit_set_mouse_auto_suspended(env, eb, false, false);
                } else {
                    // Ignore trailing motion and duplicate releases from the drag.
                    return true;
                }
            }
        }
    }

    if (!eb->mouse_reporting_auto_suspended && !eb->mouse_terminal_selection_suspended) {
        return false;
    }

    if (edit_key_is_mouse_toggle_binding(env, key)) {
        return false;
    }

    if (edit_mouse_auto_resume_triggered_by_key(key) ||
        (!eb->mouse_reporting_auto_suspended &&
         (key_no_mods == KEY_EVENT_MOUSE_WHEEL_UP || key_no_mods == KEY_EVENT_MOUSE_WHEEL_DOWN))) {
        edit_set_mouse_auto_suspended(env, eb, false, false);
    }
    return false;
}

static bool edit_mouse_event_is_drag(editor_t* eb, const tty_mouse_event_t* event) {
    if (eb->mouse_reporting_mode != IC_MOUSE_CLICKING_SMART) {
        return false;
    }
    if (event->column <= 0 || event->row <= 0) {
        eb->mouse_left_button_down = false;
        return false;
    }
    if (event->action == TTY_MOUSE_ACTION_LEFT_PRESS) {
        eb->mouse_left_button_down = true;
        eb->mouse_left_press_column = event->column;
        eb->mouse_left_press_row = event->row;
        return false;
    }

    const bool moved =
        (!eb->mouse_left_button_down || event->column != eb->mouse_left_press_column ||
         event->row != eb->mouse_left_press_row);
    if (event->action == TTY_MOUSE_ACTION_LEFT_DRAG) {
        return moved;
    }

    // Some terminals/multiplexers only report presses and releases. A release in
    // another cell still identifies a drag, though selection needs a new gesture.
    const bool dragged =
        (event->action == TTY_MOUSE_ACTION_LEFT_RELEASE && eb->mouse_left_button_down && moved);
    eb->mouse_left_button_down = false;
    return dragged;
}

static bool edit_mouse_event_starts_terminal_selection(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || env->tty == NULL) {
        return false;
    }

    tty_mouse_event_t mouse_event;
    if (!tty_get_last_mouse_event(env->tty, &mouse_event)) {
        return false;
    }

    if (edit_mouse_event_is_drag(eb, &mouse_event)) {
        // Release capture on the first moved cell. Whether the terminal continues
        // this same drag as a native selection depends on its mouse handling.
        return true;
    }

    if (mouse_event.action != TTY_MOUSE_ACTION_LEFT_PRESS &&
        mouse_event.action != TTY_MOUSE_ACTION_LEFT_RELEASE) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col = 0;
    bool clicked_above_viewport = false;
    const bool has_target = edit_mouse_event_to_target_rowcol(env, eb, &mouse_event, &target_row,
                                                              &target_col, &clicked_above_viewport);
    if (clicked_above_viewport) {
        return true;
    }
    if (!has_target) {
        return false;
    }

    // Status and other non-menu helper rows are display-only. Let a drag that starts there select
    // terminal text instead of keeping click-to-position capture active.
    if (target_row >= eb->input_rows) {
        return eb->mouse_terminal_selection_extra;
    }

    // The left prompt and continuation/line-number gutters are not editable text either. A click
    // in the input columns remains a cursor-positioning click; a press in these prefix columns
    // hands control back to the terminal's selection behavior.
    ssize_t promptw = 0;
    ssize_t cpromptw = 0;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);
    const ssize_t row_prompt_width = (target_row == 0 ? promptw : cpromptw);
    return (target_col < row_prompt_width);
}

static bool edit_should_auto_suspend_mouse_reporting(ic_env_t* env, editor_t* eb, code_t key,
                                                     bool* terminal_selection) {
    if (terminal_selection != NULL) {
        *terminal_selection = false;
    }
    if (env == NULL || eb == NULL) {
        return false;
    }

    if (eb->mouse_reporting_mode != IC_MOUSE_CLICKING_SMART || !eb->mouse_reporting_enabled ||
        !eb->mouse_reporting_manual_enabled || eb->mouse_reporting_auto_suspended) {
        return false;
    }

    code_t key_no_mods = KEY_NO_MODS(key);
    if (key_no_mods == KEY_EVENT_MOUSE_WHEEL_UP || key_no_mods == KEY_EVENT_MOUSE_WHEEL_DOWN) {
        return true;
    }

    if (key_no_mods == KEY_EVENT_MOUSE_OTHER &&
        edit_mouse_event_starts_terminal_selection(env, eb)) {
        if (terminal_selection != NULL) {
            *terminal_selection = true;
        }
        return true;
    }

    return false;
}

static void edit_toggle_mouse_reporting(ic_env_t* env, editor_t* eb) {
    if (eb == NULL) {
        return;
    }

    if (!edit_mouse_mode_supports_editing_capture(eb->mouse_reporting_mode)) {
        return;
    }

    bool currently_enabled = edit_mouse_capture_should_be_enabled(eb);
    edit_set_mouse_manual_enabled(env, eb, !currently_enabled);
}

static void edit_reset_mouse_reporting_session(ic_env_t* env, editor_t* eb, bool apply_default) {
    if (eb == NULL) {
        return;
    }

    eb->mouse_left_button_down = false;

    if (env != NULL && env->tty != NULL) {
        tty_clear_last_mouse_event(env->tty);
    }

    if (!apply_default) {
        eb->mouse_reporting_mode = IC_MOUSE_CLICKING_DISABLED;
        eb->mouse_reporting_manual_enabled = false;
        eb->mouse_reporting_auto_suspended = false;
        eb->mouse_terminal_selection_suspended = false;
        edit_apply_mouse_reporting_policy(env, eb);
        edit_set_mouse_focus_reporting(env, eb, false);
        return;
    }

    ic_mouse_clicking_mode_t mode = IC_MOUSE_CLICKING_SMART;
    bool default_enabled = true;
    if (env != NULL) {
        mode = env->mouse_reporting_mode;
        default_enabled = env->mouse_reporting_enabled_by_default;
    }

    eb->mouse_reporting_mode = edit_normalize_mouse_mode(mode);
    eb->mouse_reporting_manual_enabled =
        (default_enabled && edit_mouse_mode_supports_editing_capture(eb->mouse_reporting_mode));
    eb->mouse_reporting_auto_suspended = false;
    eb->mouse_terminal_selection_suspended = false;
    edit_apply_mouse_reporting_policy(env, eb);
    edit_set_mouse_focus_reporting(env, eb, (eb->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART));
}

static bool edit_enable_menu_mouse_scroll(ic_env_t* env) {
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;
    if (edit_normalize_mouse_mode(eb->mouse_reporting_mode) == IC_MOUSE_CLICKING_DISABLED) {
        return false;
    }

    if (!edit_mouse_mode_is_menu_only(eb->mouse_reporting_mode) &&
        !edit_mouse_capture_should_be_enabled(eb)) {
        return false;
    }

    return edit_enable_mouse_tracking(env, eb);
}

static void edit_disable_menu_mouse_scroll(ic_env_t* env, bool enabled) {
    if (env == NULL) {
        return;
    }
    edit_disable_mouse_tracking(env, env->current_editor, enabled);
}

//-------------------------------------------------------------
// Menu helpers
//-------------------------------------------------------------

#include "editline_menu.c"

//-------------------------------------------------------------
// History
//-------------------------------------------------------------

#include "editline_history.c"

//-------------------------------------------------------------
// Command palette
//-------------------------------------------------------------

#include "editline_command_palette.c"

//-------------------------------------------------------------
// Application-provided menus
//-------------------------------------------------------------

#include "editline_custom_menu.c"

//-------------------------------------------------------------
// Completion
//-------------------------------------------------------------

#include "editline_completion.c"

//-------------------------------------------------------------
// Edit line: main edit loop
//-------------------------------------------------------------

static bool apply_default_multiline_start_lines(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || eb->input == NULL || env->singleline_only) {
        return false;
    }

    size_t desired = env->multiline_start_line_count;
    if (desired <= 1) {
        return false;
    }

    if (sbuf_len(eb->input) > 0) {
        return false;
    }

    const size_t max_lines = 256;
    if (desired > max_lines) {
        desired = max_lines;
    }

    bool appended = false;
    for (size_t i = 1; i < desired; ++i) {
        if (sbuf_append_char(eb->input, '\n') < 0) {
            break;
        }
        appended = true;
    }

    return appended;
}

static bool insert_initial_input(const char* initial_input, editor_t* eb, size_t cursor_pos,
                                 bool cursor_pos_set) {
    if (initial_input == NULL) {
        return false;
    }

    ssize_t length = ic_strlen(initial_input);
    bool has_trailing_enter = false;
    while (!cursor_pos_set && length > 0) {
        char ch = initial_input[length - 1];
        if (ch == '\n' || ch == '\r') {
            has_trailing_enter = true;
            length--;
        } else {
            break;
        }
    }

    sbuf_clear(eb->input);
    if (length > 0) {
        (void)sbuf_append_n(eb->input, initial_input, length);
    }
    eb->pos = sbuf_len(eb->input);
    if (cursor_pos_set && cursor_pos < to_size_t(eb->pos)) {
        eb->pos = to_ssize_t(cursor_pos);
    }
    return has_trailing_enter;
}

static bool edit_update_status_message(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || eb->status == NULL) {
        return false;
    }

    // A bracketed paste already batches redraws. Batch status callbacks too:
    // they may parse the input or search the filesystem for every character.
    // The next loop iteration after the paste ends updates the complete input.
    if (eb->refresh_suppressed) {
        return false;
    }

    const char* custom_message = NULL;
    if (env->status_message_callback != NULL) {
        const char* input_text = sbuf_string(eb->input);
        custom_message = env->status_message_callback(input_text != NULL ? input_text : "",
                                                      env->status_message_arg);
        if (custom_message != NULL && custom_message[0] == '\0') {
            custom_message = NULL;
        }
    }

    char spell_buffer[EDIT_STATUS_HINT_BUFFER_LEN];
    const char* spell_message = NULL;
    if (edit_format_spell_status_hint(env, eb, spell_buffer, sizeof(spell_buffer))) {
        spell_message = spell_buffer;
    }
    stringbuf_t* custom_combined = NULL;
    if (spell_message != NULL) {
        if (custom_message == NULL) {
            custom_message = spell_message;
        } else {
            custom_combined = sbuf_new(eb->mem);
            if (custom_combined != NULL) {
                (void)sbuf_append(custom_combined, custom_message);
                ssize_t custom_len = sbuf_len(custom_combined);
                if (custom_len > 0) {
                    const char* existing = sbuf_string(custom_combined);
                    if (existing != NULL && existing[custom_len - 1] != '\n') {
                        (void)sbuf_append_char(custom_combined, '\n');
                    }
                }
                (void)sbuf_append(custom_combined, spell_message);
                custom_message = sbuf_string(custom_combined);
            }
        }
    }

    const bool has_custom_message = (custom_message != NULL);
    const bool input_empty = (eb->input == NULL ? true : (sbuf_len(eb->input) == 0));
    const ic_status_hint_mode_t mode = env->status_hint_mode;

    bool request_default = false;
    bool combine_with_custom = false;
    switch (mode) {
        case IC_STATUS_HINT_OFF:
            request_default = false;
            break;
        case IC_STATUS_HINT_NORMAL:
            request_default = (!has_custom_message && input_empty);
            break;
        case IC_STATUS_HINT_TRANSIENT:
            request_default = !has_custom_message;
            break;
        case IC_STATUS_HINT_PERSISTENT:
            request_default = true;
            combine_with_custom = has_custom_message;
            break;
        default:
            request_default = !has_custom_message;
            break;
    }

    char fallback_buffer[EDIT_STATUS_HINT_BUFFER_LEN];
    char mouse_status_text[EDIT_STATUS_HINT_BUFFER_LEN];
    char mouse_status_bbcode[EDIT_STATUS_HINT_BUFFER_LEN];
    const char* mouse_status_prefix = "[ic-status]Mouse clicking is enabled[/]";
    const char* next = custom_message;
    stringbuf_t* combined = NULL;
    stringbuf_t* with_mouse_prefix = NULL;

    if (request_default) {
        if (edit_format_default_status_hints(env, fallback_buffer, sizeof(fallback_buffer))) {
            if (combine_with_custom && has_custom_message) {
                combined = sbuf_new(eb->mem);
                if (combined != NULL) {
                    (void)sbuf_append(combined, fallback_buffer);
                    if (custom_message[0] != '\0') {
                        (void)sbuf_append_char(combined, '\n');
                        (void)sbuf_append(combined, custom_message);
                    }
                    next = sbuf_string(combined);
                } else {
                    next = custom_message;
                }
            } else {
                next = fallback_buffer;
            }
        } else if (!has_custom_message) {
            if (sbuf_len(eb->status) > 0) {
                sbuf_clear(eb->status);
                return true;
            }
            return false;
        }
    }

    if ((eb->mouse_reporting_enabled || eb->mouse_terminal_selection_suspended) &&
        env->mouse_reporting_status_line_enabled) {
        edit_format_mouse_enabled_status_hint(env, true, mouse_status_text,
                                              sizeof(mouse_status_text));
        if (snprintf(mouse_status_bbcode, sizeof(mouse_status_bbcode), "[ic-status]%s[/]",
                     mouse_status_text) >= 0) {
            mouse_status_prefix = mouse_status_bbcode;
        }

        with_mouse_prefix = sbuf_new(eb->mem);
        if (with_mouse_prefix != NULL) {
            (void)sbuf_append(with_mouse_prefix, mouse_status_prefix);
            if (next != NULL && next[0] != '\0') {
                (void)sbuf_append_char(with_mouse_prefix, '\n');
                (void)sbuf_append(with_mouse_prefix, next);
            }
            next = sbuf_string(with_mouse_prefix);
        } else if (next == NULL || next[0] == '\0') {
            next = mouse_status_prefix;
        }
    }

    bool changed = false;
    const char* current = sbuf_string(eb->status);
    if (next == NULL) {
        if (sbuf_len(eb->status) > 0) {
            sbuf_clear(eb->status);
            changed = true;
        }
    } else if (current == NULL || strcmp(current, next) != 0) {
        sbuf_replace(eb->status, next);
        changed = true;
    }

    if (combined != NULL) {
        sbuf_free(combined);
    }
    if (custom_combined != NULL) {
        sbuf_free(custom_combined);
    }
    if (with_mouse_prefix != NULL) {
        sbuf_free(with_mouse_prefix);
    }

    return changed;
}

static bool edit_should_submit_current_buffer(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return true;
    }

    ic_check_for_continuation_or_return_fun_t* callback = env->continuation_check_callback;
    if (callback == NULL) {
        return true;
    }

    const char* buffer = sbuf_string(eb->input);
    if (buffer == NULL) {
        buffer = "";
    }

    return callback(buffer, env->continuation_check_arg);
}

static void edit_release_editor(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return;
    }
    editstate_done(env->mem, &eb->undo);
    editstate_done(env->mem, &eb->redo);
    attrbuf_free(eb->attrs);
    attrbuf_free(eb->attrs_extra);
    sbuf_free(eb->input);
    sbuf_free(eb->extra);
    sbuf_free(eb->status);
    sbuf_free(eb->hint);
    sbuf_free(eb->hint_help);
    sbuf_free(eb->history_prefix);
    mem_free(env->mem, eb->rendered_hint_snapshot);
    mem_free(env->mem, (void*)eb->prompt_text);
    mem_free(env->mem, eb->prompt_prefix_text);
}

static char* edit_line(ic_env_t* env, const char* prompt_text, const char* inline_right_text) {
    env->last_readline_disposition = IC_READLINE_DISPOSITION_ERROR;

    // set up an edit buffer
    editor_t eb;
    memset(&eb, 0, sizeof(eb));
    eb.mem = env->mem;
    eb.input = sbuf_new(env->mem);
    eb.extra = sbuf_new(env->mem);
    eb.status = sbuf_new(env->mem);
    eb.hint = sbuf_new(env->mem);
    eb.hint_help = sbuf_new(env->mem);
    eb.history_prefix = sbuf_new(env->mem);
    eb.termw = term_get_width(env->term);
    eb.termh = term_get_height(env->term);
    eb.pos = 0;
    eb.cur_rows = 1;
    eb.input_rows = 1;
    eb.cur_row = 0;
    eb.view_rows = 1;
    eb.view_input_rows = 1;
    eb.modified = false;

    const char* original_prompt = (prompt_text != NULL ? prompt_text : "");
    bool line_has_content = term_line_has_visible_content(env->term);
    bool cursor_at_line_start = (!line_has_content || term_is_cursor_at_line_start(env->term));
    if (!line_has_content && env->tty != NULL && tty_input_pending(env->tty)) {
        line_has_content = true;
        cursor_at_line_start = false;
    }
    if (line_has_content && !cursor_at_line_start) {
        ssize_t eol_width = 0;
        const char* eol_mark = env->prompt_eol_mark;

        if (eol_mark != NULL) {
            if (env->bbcode != NULL) {
                eol_width = bbcode_column_width(env->bbcode, eol_mark);
                if (eol_width < 0) {
                    eol_width = 0;
                }
                if (eol_mark[0] != '\0') {
                    bbcode_print(env->bbcode, eol_mark);
                }
            } else {
                eol_width = ic_strlen(eol_mark);
                term_write(env->term, eol_mark);
            }
        } else {
            char fallback_mark = '%';
#if !defined(_WIN32)
            if (geteuid() == 0) {
                fallback_mark = '#';
            }
#endif

            attr_t eol_attr = attr_default();
            eol_attr.x.bold = IC_ON;
            eol_attr.x.reverse = IC_ON;
            term_set_attr(env->term, eol_attr);
            term_write_char(env->term, fallback_mark);
            term_attr_reset(env->term);
            eol_width = 1;
        }

        ssize_t termw = term_get_width(env->term);
        ssize_t pad = termw - eol_width;
        if (pad < 0) {
            pad = 0;
        }
        if (pad > 0) {
            term_write_repeat(env->term, " ", pad);
        }
        term_write_char(env->term, '\r');
        term_write_repeat(env->term, " ", eol_width);
        term_write_char(env->term, '\r');
    }

    term_set_track_output(env->term, false);

    // Initialize unused rows with the default background before drawing a new prompt.
    term_attr_reset(env->term);
    term_clear_lines_to_end(env->term);

    if (!env->bracketed_paste_enabled && env->term != NULL && term_is_interactive(env->term)) {
        term_write(env->term, "\x1b[?2004h");
        env->bracketed_paste_enabled = true;
    }

    // Handle multi-line prompts: print prefix lines and use only the last line
    // as the prompt
    eb.prompt_prefix_lines = print_prompt_prefix_lines(env, &eb, original_prompt);
    eb.prompt_begins_with_newline = (original_prompt[0] == '\n');
    char* last_line_prompt = extract_last_prompt_line(env->mem, original_prompt);
    eb.prompt_text = last_line_prompt;
    eb.replace_prompt_line_with_number = prompt_line_should_use_line_numbers(env, &eb);

    eb.inline_right_text = inline_right_text;
    eb.inline_right_width = 0;
    eb.line_number_column_width = 0;

    eb.history_idx = 0;
    editstate_init(&eb.undo);
    editstate_init(&eb.redo);

    // Set this editor as the current active editor
    env->current_editor = &eb;
    // Catch events that arrived before this readline installed its wakeup channel.
    env->readline_event_pending = (env->readline_event_callback != NULL);
    edit_reset_mouse_reporting_session(env, &eb, true);

    // Insert initial input if present
    bool seeded_multiline_lines = false;
    bool initial_requests_submit = false;

    if (env->initial_input != NULL) {
        initial_requests_submit = insert_initial_input(
            env->initial_input, &eb, env->initial_cursor_pos, env->initial_cursor_pos_set);
        // Expand pending abbreviations in pre-seeded buffers (e.g., typeahead with trailing
        // space). A caller-supplied cursor means this is an exact editor-state restore.
        if (!env->initial_cursor_pos_set) {
            (void)edit_expand_abbreviation_if_needed(env, &eb, false);
        }
    } else {
        seeded_multiline_lines = apply_default_multiline_start_lines(env, &eb);
    }

    if (eb.input == NULL || eb.extra == NULL || eb.status == NULL || eb.hint == NULL ||
        eb.hint_help == NULL || eb.history_prefix == NULL) {
        env->current_editor = NULL;
        edit_release_editor(env, &eb);
        return NULL;
    }

    // caching
    if (!(env->no_highlight && env->no_bracematch)) {
        eb.attrs = attrbuf_new(env->mem);
        eb.attrs_extra = attrbuf_new(env->mem);
    }

    // show prompt
    edit_write_prompt(env, &eb, 0, false, 0, 0, 0, false);

    // Force refresh if initial input was provided to display it immediately
    if (env->initial_input != NULL) {
        edit_refresh(env, &eb);
    } else if (inline_right_text != NULL || seeded_multiline_lines) {
        edit_refresh(env, &eb);
    }

    // always a history entry for the current input
    history_begin_edit(env->history);

    if (edit_update_status_message(env, &eb)) {
        if (eb.refresh_suppressed) {
            eb.refresh_pending = true;
        } else {
            edit_refresh(env, &eb);
        }
    }

    // process keys
    code_t c = KEY_NONE;  // current key code
    bool has_pending_key = false;
    code_t pending_key = KEY_NONE;
    bool ctrl_c_pressed = false;
    bool ctrl_d_pressed = false;
    bool stop_event_received = false;
    bool idle_timeout_received = false;
    bool hint_delay_satisfied = false;
    edit_note_input_activity(env, &eb);

edit_loop_entry:
    if (!initial_requests_submit) {
        while (true) {
            edit_process_readline_event(env);
            if (edit_update_status_message(env, &eb)) {
                if (eb.refresh_suppressed) {
                    eb.refresh_pending = true;
                } else {
                    edit_refresh(env, &eb);
                }
            }
            edit_flush_notifications(env, &eb);

            if (eb.request_submit) {
                // Clear history preview when submitting
                edit_clear_history_preview(&eb);
                if (edit_expand_abbreviation_if_needed(env, &eb, false)) {
                    edit_refresh(env, &eb);
                }
                bool should_submit = edit_should_submit_current_buffer(env, &eb);
                if (!should_submit && !env->singleline_only) {
                    eb.request_submit = false;
                    edit_insert_auto_indented_linefeed(env, &eb);
                    continue;
                }
                if (should_submit && edit_try_spell_correct_on_enter(env, &eb)) {
                    should_submit = edit_should_submit_current_buffer(env, &eb);
                    if (!should_submit && !env->singleline_only) {
                        eb.request_submit = false;
                        edit_insert_auto_indented_linefeed(env, &eb);
                        continue;
                    }
                }
                c = KEY_ENTER;
                break;
            }

            if (has_pending_key) {
                c = pending_key;
                has_pending_key = false;
            } else {
                // read a character
                term_flush(env->term);
                if (env->idle_timeout > 0) {
                    while (true) {
                        long idle_remaining = edit_milliseconds_until(eb.idle_deadline_ms);
                        if (idle_remaining == 0) {
                            idle_timeout_received = true;
                            c = KEY_NONE;
                            break;
                        }

                        long wait_ms = idle_remaining;
                        bool waiting_for_hint =
                            (!hint_delay_satisfied && env->hint_delay > 0 && sbuf_len(eb.hint) > 0);
                        int64_t hint_deadline_ms = eb.last_activity_ms + (int64_t)env->hint_delay;
                        if (waiting_for_hint) {
                            long hint_remaining = edit_milliseconds_until(hint_deadline_ms);
                            if (hint_remaining == 0) {
                                edit_refresh(env, &eb);
                                hint_delay_satisfied = true;
                                continue;
                            }
                            if (hint_remaining < wait_ms) {
                                wait_ms = hint_remaining;
                            }
                        }

                        if (tty_read_timeout(env->tty, wait_ms, &c)) {
                            if (c != KEY_EVENT_RESIZE && c != KEY_EVENT_READLINE) {
                                edit_note_input_activity(env, &eb);
                                hint_delay_satisfied = false;
                                if (waiting_for_hint) {
                                    sbuf_clear(eb.hint);
                                    sbuf_clear(eb.hint_help);
                                }
                            }
                            break;
                        }

                        if (edit_milliseconds_until(eb.idle_deadline_ms) == 0) {
                            idle_timeout_received = true;
                            c = KEY_NONE;
                            break;
                        }
                        if (waiting_for_hint && edit_milliseconds_until(hint_deadline_ms) == 0) {
                            edit_refresh(env, &eb);
                            hint_delay_satisfied = true;
                            continue;
                        }

                        // An asynchronous terminal notification woke the reader.
                        // Let the normal resize/event path process it without
                        // treating it as user activity.
                        c = KEY_NONE;
                        break;
                    }
                } else if (env->hint_delay <= 0 || sbuf_len(eb.hint) == 0) {
                    // blocking read
                    c = tty_read(env->tty);
                } else {
                    // timeout to display hint
                    if (!tty_read_timeout(env->tty, env->hint_delay, &c)) {
                        // timed-out
                        if (sbuf_len(eb.hint) > 0) {
                            // display hint
                            edit_refresh(env, &eb);
                        }
                        c = tty_read(env->tty);
                    } else if (c != KEY_EVENT_READLINE) {
                        // clear the pending hint if we got input before the delay
                        // expired
                        sbuf_clear(eb.hint);
                        sbuf_clear(eb.hint_help);
                    }
                }
            }

            if (idle_timeout_received) {
                break;
            }

            // update terminal in case of a resize (also detect polling changes)
            bool should_resize = tty_term_resize_event(env->tty);
            if (!should_resize && term_update_dim(env->term)) {
                should_resize = true;
            }
            if (should_resize) {
                (void)edit_resize(env, &eb);
            }

            if (c == KEY_EVENT_READLINE) {
                env->readline_event_pending = true;
                continue;
            }

            if (edit_maybe_resume_smart_mouse_reporting(env, &eb, c)) {
                continue;
            }

            // clear hint only after a potential resize (so resize row calculations
            // are correct)
            const bool had_hint = (sbuf_len(eb.hint) > 0);
            char* pending_hint = (had_hint ? sbuf_strdup(eb.hint) : NULL);
            sbuf_clear(eb.hint);
            sbuf_clear(eb.hint_help);

            // Preserve the passive layout for mouse hit testing and resize events. Keyboard
            // input dismisses the old rendering; editing operations rebuild it for the new buffer.
            const code_t base_key = KEY_NO_MODS(c);
            if (eb.completion_auto_menu_visible && c != KEY_NONE && c != KEY_EVENT_RESIZE &&
                base_key != KEY_EVENT_MOUSE_OTHER && base_key != KEY_EVENT_MOUSE_WHEEL_UP &&
                base_key != KEY_EVENT_MOUSE_WHEEL_DOWN && base_key != KEY_EVENT_FOCUS_IN &&
                base_key != KEY_EVENT_FOCUS_OUT) {
                eb.completion_auto_menu_visible = false;
                sbuf_clear(eb.extra);
                completions_clear(env->completions);
                if (code_is_virt_key(c)) {
                    edit_refresh(env, &eb);
                }
                if (c == KEY_ESC) {
                    mem_free(eb.mem, pending_hint);
                    continue;  // hide suggestions, not the input; the next edit reopens them
                }
            }

            bool request_submit = false;

            if (c == KEY_CTRL_O) {
                c = KEY_ENTER;
            }

            if (edit_key_resets_last_arg_state(env, c)) {
                edit_reset_last_arg_state(&eb);
            }

            // if the user tries to move into a hint with right-cursor or end, either
            // materialize it or fall back to completion logic
            if ((c == KEY_RIGHT || c == KEY_END) && had_hint) {
                bool allow_force_completion = (c == KEY_END) || edit_pos_is_at_row_end(env, &eb);
                if (allow_force_completion) {
                    bool spell_hint = false;
                    if (pending_hint != NULL && completions_count(env->completions) > 0) {
                        const char* source = completions_get_source(env->completions, 0);
                        spell_hint = (source != NULL && strcmp(source, "spell") == 0);
                    }
                    if (pending_hint != NULL && editor_pos_is_at_end(&eb) && !spell_hint) {
                        // Apply the inline hint directly when already at the end of the input
                        editor_start_modify(&eb);
                        ssize_t new_pos = sbuf_insert_at(eb.input, pending_hint, eb.pos);
                        if (new_pos >= 0) {
                            eb.pos = new_pos;
                        }
                        edit_refresh_hint(env, &eb);
                        mem_free(eb.mem, pending_hint);
                        pending_hint = NULL;
                        continue;
                    }
                    edit_generate_completions(env, &eb, true);
                    c = KEY_NONE;
                }
            }

            if ((c < IC_KEY_EVENT_BASE || c >= IC_KEY_UNICODE_MAX) &&
                key_binding_execute(env, &eb, c)) {
                if (pending_hint != NULL) {
                    mem_free(eb.mem, pending_hint);
                    pending_hint = NULL;
                }
                continue;
            }

            if (edit_key_is_mouse_toggle_binding(env, c)) {
                edit_toggle_mouse_reporting(env, &eb);
                if (pending_hint != NULL) {
                    mem_free(eb.mem, pending_hint);
                    pending_hint = NULL;
                }
                continue;
            }

            bool terminal_selection = false;
            if (edit_should_auto_suspend_mouse_reporting(env, &eb, c, &terminal_selection)) {
                edit_set_mouse_auto_suspended(env, &eb, true, terminal_selection);
                if (pending_hint != NULL) {
                    mem_free(eb.mem, pending_hint);
                    pending_hint = NULL;
                }
                continue;
            }

            if (eb.mouse_reporting_enabled && KEY_NO_MODS(c) == KEY_EVENT_MOUSE_OTHER &&
                (edit_activate_completion_auto_menu_on_click(env, &eb) ||
                 edit_handle_mouse_click(env, &eb, pending_hint))) {
                if (pending_hint != NULL) {
                    mem_free(eb.mem, pending_hint);
                    pending_hint = NULL;
                }
                continue;
            }

            if (pending_hint != NULL) {
                mem_free(eb.mem, pending_hint);
                pending_hint = NULL;
            }

            // Operations that may return
            if (c == KEY_ENTER) {
                // Clear history preview when submitting
                edit_clear_history_preview(&eb);
                if (!env->singleline_only && editor_pos_is_at_end(&eb) && eb.pos > 0 &&
                    sbuf_string(eb.input)[eb.pos - 1] == env->multiline_eol &&
                    edit_pos_is_at_row_end(env, &eb)) {
                    // At the end of the buffer, start a new row after a line-continuation.
                    // A continuation on an earlier line is already followed by content, so Enter
                    // there submits the complete buffer instead of adding another continuation.
                    edit_multiline_eol(env, &eb);
                } else {
                    // otherwise done
                    if (edit_expand_abbreviation_if_needed(env, &eb, false)) {
                        edit_refresh(env, &eb);
                    }
                    request_submit = true;
                }
            } else if (c == KEY_CTRL_D) {
                if (eb.pos == 0 && editor_pos_is_at_end(&eb)) {
                    ctrl_d_pressed = true;
                    break;  // ctrl+D on empty quits with CTRL+D token
                }
                edit_delete_char(env, &eb);  // otherwise it is like delete
            } else if (c == KEY_CTRL_C || c == KEY_EVENT_STOP || c == KEY_EVENT_INTERRUPT) {
                // Clear history preview when cancelling
                edit_clear_history_preview(&eb);
                if (c == KEY_EVENT_STOP) {
                    stop_event_received = true;
                }
                ctrl_c_pressed = true;
                break;  // ctrl+C or STOP event quits with CTRL+C token
            } else if (c == KEY_ESC) {
                // Clear history preview on ESC
                edit_clear_history_preview(&eb);
                if (eb.pos == 0 && editor_pos_is_at_end(&eb)) {
                    // Keep the prompt in place when ESC is pressed on an empty buffer.
                    continue;
                }
                edit_delete_all(env, &eb);  // otherwise delete the current input
                // edit_delete_line(env,&eb);  // otherwise delete the current line
            } else if (c == KEY_BELL /* ^G */) {
                edit_delete_all(env, &eb);
                break;  // ctrl+G cancels (and returns empty input)
            }

            // Editing Operations
            else {
                switch (c) {
                        // events
                    case KEY_EVENT_RESIZE:
                        (void)edit_resize(env, &eb);
                        break;
                    case KEY_EVENT_AUTOTAB:
                        edit_generate_completions(env, &eb, true);
                        break;
                    case IC_KEY_PASTE_START:  // bracketed paste start marker
                        eb.refresh_suppressed = true;
                        eb.refresh_pending = false;
                        break;
                    case IC_KEY_PASTE_END:  // bracketed paste end marker
                        eb.refresh_suppressed = false;
                        if (eb.refresh_pending) {
                            eb.refresh_pending = false;
                            if (env->completion_auto_menu) {
                                edit_refresh_hint(env, &eb);
                            } else {
                                edit_refresh(env, &eb);
                            }
                        }
                        break;

                    // completion, history, help, undo
                    case KEY_TAB:
                    case WITH_ALT('?'):
                        edit_generate_completions(env, &eb, false);
                        break;
                    case KEY_CTRL_R:
                    case KEY_CTRL_S:
                        edit_history_search_with_current_line(env, &eb);
                        break;
                    case WITH_ALT('p'):
                    case WITH_ALT('P'):
                        edit_command_palette(env, &eb);
                        break;
                    case KEY_CTRL_P:
                        edit_history_prev(env, &eb);
                        break;
                    case KEY_CTRL_N:
                    case WITH_SHIFT(KEY_DOWN):
                        edit_history_next(env, &eb);
                        break;
                    case KEY_CTRL_L:
                        edit_clear_screen(env, &eb);
                        break;
                    case KEY_CTRL_Z:
                    case WITH_CTRL('_'):
                        edit_undo_restore(env, &eb);
                        break;
                    case KEY_CTRL_Y:
                        edit_redo_restore(env, &eb);
                        break;
                    case KEY_F1:
                        edit_show_help(env, &eb);
                        break;

                    // navigation
                    case KEY_LEFT:
                    case KEY_CTRL_B:
                        edit_cursor_left(env, &eb);
                        break;
                    case KEY_RIGHT:
                    case KEY_CTRL_F:
                        if (!env->completion_auto_menu && eb.pos == sbuf_len(eb.input) &&
                            edit_pos_is_at_row_end(env, &eb)) {
                            edit_generate_completions(env, &eb, false);
                        } else {
                            edit_cursor_right(env, &eb);
                        }
                        break;
                    case KEY_UP:
                        edit_cursor_row_up(env, &eb);
                        break;
                    case WITH_SHIFT(KEY_UP):
                        edit_history_prev_with_spell(env, &eb);
                        break;
                    case KEY_DOWN:
                        edit_cursor_row_down(env, &eb);
                        break;
                    case KEY_HOME:
                        edit_cursor_line_start(env, &eb);
                        break;
                    case KEY_CTRL_A:
                        edit_cursor_ctrl_a(env, &eb);
                        break;
                    case KEY_END:
                        edit_cursor_line_end(env, &eb);
                        break;
                    case KEY_CTRL_E:
                        edit_cursor_ctrl_e(env, &eb);
                        break;
                    case KEY_CTRL_LEFT:
                    case WITH_SHIFT(KEY_LEFT):
                    case WITH_ALT('b'):
                        edit_cursor_prev_word(env, &eb);
                        break;
                    case KEY_CTRL_RIGHT:
                    case WITH_SHIFT(KEY_RIGHT):
                    case WITH_ALT('f'):
                        if (!env->completion_auto_menu && eb.pos == sbuf_len(eb.input) &&
                            edit_pos_is_at_row_end(env, &eb)) {
                            edit_generate_completions(env, &eb, false);
                        } else {
                            edit_cursor_next_word(env, &eb);
                        }
                        break;
                    case KEY_CTRL_HOME:
                    case WITH_SHIFT(KEY_HOME):
                    case KEY_PAGEUP:
                    case WITH_ALT('<'):
                        edit_cursor_to_start(env, &eb);
                        break;
                    case KEY_CTRL_END:
                    case WITH_SHIFT(KEY_END):
                    case KEY_PAGEDOWN:
                    case WITH_ALT('>'):
                        edit_cursor_to_end(env, &eb);
                        break;
                    case WITH_ALT('m'):
                        edit_cursor_match_brace(env, &eb);
                        break;

                    // deletion
                    case KEY_BACKSP:
                        edit_backspace(env, &eb);
                        break;
                    case KEY_DEL:
                        edit_delete_char(env, &eb);
                        break;
                    case WITH_ALT('d'):
                        edit_delete_to_end_of_word(env, &eb);
                        break;
                    case KEY_CTRL_W:
                        edit_delete_to_start_of_ws_word(env, &eb);
                        break;
                    case WITH_ALT(KEY_DEL):
                    case WITH_ALT(KEY_BACKSP):
                        edit_delete_to_start_of_word(env, &eb);
                        break;
                    case KEY_CTRL_U:
                        edit_delete_to_start_of_line(env, &eb);
                        break;
                    case KEY_CTRL_K:
                        edit_delete_to_end_of_line(env, &eb);
                        break;
                    case KEY_CTRL_T:
                        edit_swap_char(env, &eb);
                        break;
                    case WITH_ALT('.'):
                    case WITH_ALT('_'):
                        edit_yank_last_arg(env, &eb);
                        break;

                    // Editing
                    case KEY_SHIFT_TAB:
                    case KEY_LINEFEED:  // '\n' (ctrl+J, shift+enter)
                        if (!env->singleline_only) {
                            edit_insert_char(env, &eb, '\n');
                        }
                        break;
                    default: {
                        char chr;
                        unicode_t uchr;
                        if (code_is_ascii_char(c, &chr)) {
                            edit_insert_char(env, &eb, chr);
                        } else if (code_is_unicode(c, &uchr)) {
                            edit_insert_unicode(env, &eb, uchr);
                        } else {
                            // Try the unhandled key callback before ignoring
                            // bool handled = false;
                            // if (env->unhandled_key_handler != NULL) {
                            //     handled = env->unhandled_key_handler(c, env->unhandled_key_arg);
                            // }
                            // if (!handled) {
                            //     debug_msg("edit: ignore code: 0x%04x\n", c);
                            // }
                            // debug_msg("edit: ignore code: 0x%04x\n", c);
                        }
                        break;
                    }
                }
            }

            if (request_submit || eb.request_submit) {
                bool should_submit = edit_should_submit_current_buffer(env, &eb);
                if (!should_submit && !env->singleline_only) {
                    eb.request_submit = false;
                    edit_insert_auto_indented_linefeed(env, &eb);
                    continue;
                }
                if (should_submit && edit_try_spell_correct_on_enter(env, &eb)) {
                    should_submit = edit_should_submit_current_buffer(env, &eb);
                    if (!should_submit && !env->singleline_only) {
                        eb.request_submit = false;
                        edit_insert_auto_indented_linefeed(env, &eb);
                        continue;
                    }
                }
                c = KEY_ENTER;
                break;
            }
        }
    } else {
        if (!edit_should_submit_current_buffer(env, &eb) && !env->singleline_only) {
            initial_requests_submit = false;
            edit_insert_auto_indented_linefeed(env, &eb);
            goto edit_loop_entry;
        }
        (void)edit_expand_abbreviation_if_needed(env, &eb, false);
        c = KEY_ENTER;
    }

    // goto end

    edit_process_readline_event(env);
    edit_flush_notifications(env, &eb);

    if (!idle_timeout_received) {
        eb.pos = sbuf_len(eb.input);
    }

    // Final chance to expand pending abbreviations (e.g., buffered typeahead ending in space)
    if (!idle_timeout_received && !ctrl_c_pressed && !ctrl_d_pressed && c != KEY_EVENT_STOP) {
        (void)edit_expand_abbreviation_if_needed(env, &eb, false);
    }

    eb.mouse_terminal_selection_suspended = false;
    if (eb.status != NULL && sbuf_len(eb.status) > 0) {
        // Ensure status lines are cleared before handing control back to the caller
        sbuf_clear(eb.status);
    }

    if (line_numbers_enabled(env)) {
        eb.force_linear_line_numbers = true;
    }

    if (idle_timeout_received) {
        edit_clear_with_prompt_prefix(env, &eb, eb.prompt_prefix_lines);
        term_flush(env->term);
    } else {
        // refresh once more but without brace matching
        bool bm = env->no_bracematch;
        env->no_bracematch = true;
        edit_refresh(env, &eb);
        env->no_bracematch = bm;
        edit_flush_notifications(env, &eb);
    }

    // save result
    char* res;
    if (idle_timeout_received) {
        res = sbuf_strdup(eb.input);
    } else if (ctrl_d_pressed) {
        res = mem_strdup(env->mem, IC_READLINE_TOKEN_CTRL_D);
    } else if (ctrl_c_pressed) {
        res = mem_strdup(env->mem, IC_READLINE_TOKEN_CTRL_C);
    } else if ((c == KEY_CTRL_D && sbuf_len(eb.input) == 0) || c == KEY_CTRL_C ||
               c == KEY_EVENT_STOP) {
        res = NULL;
    } else if (!tty_is_utf8(env->tty)) {
        res = sbuf_strdup_from_utf8(eb.input);
    } else {
        res = sbuf_strdup(eb.input);
    }

    env->last_readline_cursor_pos = to_size_t(eb.pos);
    if (idle_timeout_received) {
        env->last_readline_disposition = IC_READLINE_DISPOSITION_IDLE;
    } else if (ctrl_d_pressed || (c == KEY_CTRL_D && sbuf_len(eb.input) == 0)) {
        env->last_readline_disposition = IC_READLINE_DISPOSITION_EOF;
    } else if (ctrl_c_pressed || c == KEY_CTRL_C) {
        env->last_readline_disposition = (stop_event_received ? IC_READLINE_DISPOSITION_STOP
                                                              : IC_READLINE_DISPOSITION_INTERRUPT);
    } else if (c == KEY_EVENT_STOP) {
        env->last_readline_disposition = IC_READLINE_DISPOSITION_STOP;
    } else if (res == NULL) {
        env->last_readline_disposition = IC_READLINE_DISPOSITION_ERROR;
    } else {
        env->last_readline_disposition = IC_READLINE_DISPOSITION_SUBMIT;
    }

    // Discard the private draft. Hosts can record the submitted command after
    // execution; standalone isocline callers retain automatic history recording.
    history_end_edit(env->history, env->last_readline_disposition == IC_READLINE_DISPOSITION_SUBMIT
                                       ? sbuf_string(eb.input)
                                       : NULL);

    edit_reset_mouse_reporting_session(env, &eb, false);

    // Clear the current editor pointer
    env->current_editor = NULL;

    edit_release_editor(env, &eb);
    return res;
}

//-------------------------------------------------------------
// Public API for buffer control during readline
//-------------------------------------------------------------

ic_public bool ic_set_buffer(const char* buffer) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;

    // Clear or set the buffer
    if (buffer == NULL) {
        sbuf_clear(eb->input);
        eb->pos = 0;
    } else {
        sbuf_replace(eb->input, buffer);
        eb->pos = sbuf_len(eb->input);  // Move cursor to end
    }

    // Mark as modified
    eb->modified = true;

    // Refresh the display
    if (!env->readline_terminal_suspended) {
        edit_refresh(env, eb);
    }

    return true;
}

ic_public const char* ic_get_buffer(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return NULL;
    }

    editor_t* eb = env->current_editor;
    return sbuf_string(eb->input);
}

ic_public bool ic_execute_key_action(ic_key_action_t action) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL || action == IC_KEY_ACTION_RUNOFF ||
        action < IC_KEY_ACTION_NONE || action >= IC_KEY_ACTION__MAX) {
        return false;
    }

    return key_action_execute(env, env->current_editor, action, KEY_NONE);
}

ic_public bool ic_get_cursor_pos(size_t* out_pos) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL || out_pos == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;
    *out_pos = (size_t)(eb->pos >= 0 ? eb->pos : 0);
    return true;
}

ic_public bool ic_set_cursor_pos(size_t pos) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;
    ssize_t len = sbuf_len(eb->input);

    // Clamp position to valid range
    if ((ssize_t)pos > len) {
        pos = (size_t)len;
    }

    eb->pos = (ssize_t)pos;
    if (!env->readline_terminal_suspended) {
        edit_refresh(env, eb);
    }
    return true;
}

ic_public bool ic_suspend_readline_terminal(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL || env->tty == NULL || env->term == NULL ||
        env->readline_terminal_suspended) {
        return false;
    }

    editor_t* eb = env->current_editor;
    env->suspended_mouse_reporting_enabled = eb->mouse_reporting_enabled;
    env->suspended_focus_reporting_enabled = eb->mouse_focus_reporting_enabled;

    edit_set_mouse_focus_reporting(env, eb, false);
    edit_set_mouse_reporting_enabled(env, eb, false);
    if (env->bracketed_paste_enabled && term_is_interactive(env->term)) {
        term_write(env->term, "\x1b[?2004l");
        env->bracketed_paste_enabled = false;
    }
    term_attr_reset(env->term);
    term_flush(env->term);

    if (env->typeahead_enabled) {
        tty_enable_typeahead_capture_mode(env->tty, false);
    }
    term_end_raw(env->term, false);
    tty_end_raw(env->tty);
    env->readline_terminal_suspended = true;
    return true;
}

ic_public bool ic_resume_readline_terminal(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL || env->tty == NULL || env->term == NULL ||
        !env->readline_terminal_suspended) {
        return false;
    }

    editor_t* eb = env->current_editor;
    tty_adopt_external_modes(env->tty);
    if (!tty_start_raw(env->tty)) {
        return false;
    }
    term_start_raw(env->term);
    if (env->typeahead_enabled) {
        tty_enable_typeahead_capture_mode(env->tty, true);
    }
    if (term_is_interactive(env->term)) {
        term_write(env->term, "\x1b[?2004h");
        env->bracketed_paste_enabled = true;
    }

    env->readline_terminal_suspended = false;
    env->readline_event_pending = (env->readline_event_callback != NULL);
    if (env->suspended_mouse_reporting_enabled) {
        edit_set_mouse_reporting_enabled(env, eb, true);
    }
    if (env->suspended_focus_reporting_enabled) {
        edit_set_mouse_focus_reporting(env, eb, true);
    }
    env->suspended_mouse_reporting_enabled = false;
    env->suspended_focus_reporting_enabled = false;

    term_reset_line_state(env->term);
    term_start_of_line(env->term);
    eb->cur_row = 0;
    eb->cur_rows = 1;
    eb->input_rows = 1;
    eb->view_first_row = 0;
    eb->view_rows = 1;
    eb->view_input_rows = 1;
    edit_write_prompt(env, eb, 0, false, 0, 0, 0, false);
    edit_refresh(env, eb);
    return true;
}

ic_public bool ic_request_submit(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;
    eb->request_submit = true;
    return true;
}

ic_public bool ic_current_loop_advance_with_prompt(const char* new_buffer,
                                                   const char* preserved_prompt,
                                                   const char* preserved_inline_right) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;
    const char* original_inline_right = eb->inline_right_text;
    stringbuf_t* original_prompt = NULL;

    if (preserved_prompt != NULL) {
        original_prompt = sbuf_new(env->mem);
        if (original_prompt == NULL) {
            return false;
        }
        if (eb->prompt_prefix_text != NULL) {
            (void)sbuf_append(original_prompt, eb->prompt_prefix_text);
        }
        (void)sbuf_append(original_prompt, eb->prompt_text);
    }

    if ((preserved_prompt != NULL || preserved_inline_right != NULL) &&
        !ic_current_loop_reset(NULL, preserved_prompt, preserved_inline_right)) {
        sbuf_free(original_prompt);
        return false;
    }

    // Leave only the prompt and real input behind. Transient menu, status, and hint rows
    // belong to the active editor and should not become terminal output.
    sbuf_clear(eb->extra);
    sbuf_clear(eb->status);
    sbuf_clear(eb->hint);
    sbuf_clear(eb->hint_help);
    edit_refresh(env, eb);

    // Move below the complete rendered editor without clearing it. Prompt prefix lines live
    // above the viewport, so the visible cursor offset is sufficient to reach its last row.
    const ssize_t visible_cursor_row = edit_view_cursor_row(eb);
    const ssize_t rendered_rows = (eb->view_rows > 0 ? eb->view_rows : 1);
    term_start_of_line(env->term);
    term_down(env->term, rendered_rows - visible_cursor_row - 1);
    term_writeln(env->term, "");

    sbuf_replace(eb->input, (new_buffer != NULL ? new_buffer : ""));
    eb->pos = sbuf_len(eb->input);
    eb->modified = true;
    eb->cur_rows = 1;
    eb->input_rows = 1;
    eb->cur_row = 0;
    eb->view_first_row = 0;
    eb->view_rows = 1;
    eb->view_input_rows = 1;

    if (original_prompt != NULL) {
        const char* prompt_text = sbuf_string(original_prompt);
        mem_free(env->mem, (void*)eb->prompt_text);
        eb->prompt_text = extract_last_prompt_line(env->mem, prompt_text);
        eb->prompt_prefix_lines = print_prompt_prefix_lines(env, eb, prompt_text);
        eb->prompt_begins_with_newline = (prompt_text[0] == '\n');
        eb->replace_prompt_line_with_number = prompt_line_should_use_line_numbers(env, eb);
    } else if (eb->prompt_prefix_lines > 0) {
        redraw_prompt_prefix_lines(env, eb);
    }
    if (preserved_inline_right != NULL) {
        eb->inline_right_text = original_inline_right;
        eb->inline_right_width = 0;
    }
    edit_write_prompt(env, eb, 0, false, 0, 0, 0, false);
    edit_refresh(env, eb);
    sbuf_free(original_prompt);
    return true;
}

ic_public bool ic_current_loop_advance(const char* new_buffer) {
    return ic_current_loop_advance_with_prompt(new_buffer, NULL, NULL);
}

ic_public bool ic_show_menu(const char* prompt_text, const ic_menu_item_t* items, size_t count,
                            size_t* selected_index) {
    return ic_show_menu_ex(prompt_text, items, count, selected_index, NULL);
}

ic_public bool ic_show_menu_ex(const char* prompt_text, const ic_menu_item_t* items, size_t count,
                               size_t* selected_index, ic_menu_accept_t* accept) {
    if (accept != NULL) {
        *accept = IC_MENU_ACCEPT_NONE;
    }
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL || env->tty == NULL || items == NULL ||
        count == 0) {
        return false;
    }

    const ssize_t item_count = to_ssize_t(count);
    if (item_count <= 0) {
        return false;
    }
    for (ssize_t i = 0; i < item_count; ++i) {
        if (items[i].label == NULL || items[i].label[0] == '\0') {
            return false;
        }
    }

    const char* prompt = (prompt_text != NULL ? prompt_text : "select: ");
    return edit_custom_menu(env, env->current_editor, prompt, items, item_count, selected_index,
                            accept);
}

ic_public bool ic_current_loop_reset(const char* new_buffer, const char* new_prompt,
                                     const char* new_inline_right) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->current_editor == NULL) {
        return false;
    }

    editor_t* eb = env->current_editor;

    // Prefix lines live outside eb->cur_rows, so clear the complete old prompt before
    // replacing any of the state used to locate it.
    const ssize_t old_prompt_prefix_lines = eb->prompt_prefix_lines;
    edit_clear_with_prompt_prefix(env, eb, old_prompt_prefix_lines);
    eb->cur_row = 0;
    eb->cur_rows = 1;
    eb->input_rows = 1;
    eb->view_first_row = 0;
    eb->view_rows = 1;
    eb->view_input_rows = 1;

    // Update buffer if provided
    if (new_buffer != NULL) {
        sbuf_replace(eb->input, new_buffer);
        eb->pos = sbuf_len(eb->input);  // Move cursor to end
        eb->modified = true;
    }

    // Update prompt if provided
    if (new_prompt != NULL) {
        // Free the old prompt text
        mem_free(env->mem, (void*)eb->prompt_text);

        // Extract and set the new prompt (handle multi-line prompts)
        char* last_line_prompt = extract_last_prompt_line(env->mem, new_prompt);
        eb->prompt_text = last_line_prompt;

        // Print prefix lines if any
        eb->prompt_prefix_lines = print_prompt_prefix_lines(env, eb, new_prompt);
        eb->prompt_begins_with_newline = (new_prompt[0] == '\n');
        eb->replace_prompt_line_with_number = prompt_line_should_use_line_numbers(env, eb);
    } else if (old_prompt_prefix_lines > 0) {
        // The display was fully cleared even when only the buffer or right prompt changed.
        redraw_prompt_prefix_lines(env, eb);
    }

    // Update inline right text if provided
    if (new_inline_right != NULL) {
        eb->inline_right_text = new_inline_right;
        eb->inline_right_width = 0;  // Will be recalculated
    }

    // Rewrite the prompt
    edit_write_prompt(env, eb, 0, false, 0, 0, 0, false);

    if (edit_update_status_message(env, eb) && eb->refresh_suppressed) {
        eb->refresh_pending = true;
    }

    // Refresh the entire display
    edit_refresh(env, eb);

    return true;
}
