/*
  env.h

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

#ifndef IC_ENV_H
#define IC_ENV_H

#include <stddef.h>

#include "bbcode.h"
#include "common.h"
#include "completions.h"
#include "history.h"
#include "isocline.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"

struct ic_keybinding_profile_s;
struct editor_s;

//-------------------------------------------------------------
// Environment
//-------------------------------------------------------------

typedef struct ic_abbreviation_entry_s {
    char* trigger;
    char* expansion;
    ssize_t trigger_len;
} ic_abbreviation_entry_t;

typedef struct ic_command_palette_entry_internal_s {
    char* id;
    char* name;
    char* description;
    char* keywords;
} ic_command_palette_entry_internal_t;

struct ic_env_s {
    alloc_t* mem;                     // potential custom allocator
    ic_env_t* next;                   // next environment (used for proper deallocation)
    term_t* term;                     // terminal
    tty_t* tty;                       // keyboard (NULL if stdin is a pipe, file, etc)
    struct editor_s* current_editor;  // pointer to active editor (NULL when not reading)
    completions_t* completions;       // current completions
    history_t* history;               // edit history
    bbcode_t* bbcode;                 // print with bbcodes
    const char* prompt_marker;        // the prompt marker (defaults to "> ")
    const char* cprompt_marker;       // prompt marker for continuation lines
                                      // (defaults to `prompt_marker`)
    char* prompt_eol_mark;            // marker shown when preserving partial pre-prompt output
    char* history_search_prompt;      // prompt shown while fuzzy searching history
    char* command_palette_prompt;     // prompt shown while searching the command palette
    char* history_search_sort_key;    // metadata key for history metadata sort modes
    ic_highlight_fun_t* highlighter;  // highlight callback
    void* highlighter_arg;            // user state for the highlighter.
    ic_unhandled_key_fun_t* unhandled_key_handler;  // callback for unhandled keys
    void* unhandled_key_arg;                        // user state for unhandled key handler
    ic_readline_event_fun_t* readline_event_callback;
    void* readline_event_arg;
    bool readline_event_pending;                       // deferred while a nested menu is open
    stringbuf_t* notifications;                        // text queued on the readline thread
    ic_status_message_fun_t* status_message_callback;  // callback for status message text
    void* status_message_arg;                          // user state for status callback
    ic_check_for_continuation_or_return_fun_t*
        continuation_check_callback;  // callback that decides whether to submit or continue
    void* continuation_check_arg;     // user state for the continuation callback
    ic_typeahead_capture_allowed_fun_t*
        typeahead_capture_allowed_callback;  // callback that gates typeahead capture
    void* typeahead_capture_allowed_arg;     // user state for the typeahead gate callback
    ic_status_hint_mode_t status_hint_mode;  // rendering behavior for default hints
    ic_mouse_clicking_mode_t
        mouse_reporting_mode;  // capture strategy for mouse interaction sessions
    ic_history_search_sort_t history_search_sort;  // default fuzzy history search sort
    bool mouse_reporting_enabled_by_default;       // should new readline sessions start with mouse
                                                   // capture active?
    bool mouse_reporting_status_line_enabled;      // show mouse-reporting indicator in the status
                                                   // line?
    const char* match_braces;                      // matching braces, e.g "()[]{}"
    const char* auto_braces;                       // auto insertion braces, e.g "()[]{}\"\"''"
    const char* initial_input;                     // initial input text to insert into editor
    size_t initial_cursor_pos;                     // cursor for pre-seeded input
    bool initial_cursor_pos_set;                   // whether the initial cursor was specified
    size_t last_readline_cursor_pos;               // cursor captured when readline returned
    stringbuf_t* typeahead_input_buffer;           // sanitized pending typeahead text
    stringbuf_t* typeahead_pending_raw_bytes;      // raw bytes awaiting replay/filtering
    ic_readline_disposition_t last_readline_disposition;  // disposition from most recent read
    char multiline_eol;                    // character used for multiline input ("\") (set to 0
                                           // to disable)
    bool retain_multiline_continuation;    // keep multiline_eol when Enter inserts a newline?
    bool initialized;                      // are we initialized?
    bool noedit;                           // is rich editing possible (tty != NULL)
    bool singleline_only;                  // allow only single line editing?
    bool complete_nopreview;               // do not show completion preview for each
                                           // selection in the completion menu?
    bool completion_auto_menu;             // show passive completions while editing (Tab activates)
    bool completion_click_accept_enabled;  // should completion clicks accept immediately?
    ic_menu_highlight_mode_t menu_highlight_mode;  // syntax highlight completion/history menus
    bool complete_autotab;                         // try to keep completing after a completion?
    bool no_multiline_indent;                      // indent continuation lines to line up under the
                                                   // initial prompt
    bool no_help;                                  // show short help line for history search etc.
    bool no_hint;                                  // allow hinting?
    bool no_highlight;                             // enable highlighting?
    bool no_bracematch;                            // enable brace matching?
    bool no_autobrace;                             // enable automatic brace insertion?
    bool no_lscolors;                              // use LSCOLORS/LS_COLORS to colorize file name
                                                   // completions?
    bool spell_correct;                            // enable spell correction on completions?
    bool spell_correct_on_enter;                   // apply single spell correction when submitting?
    bool show_line_numbers;                        // show line numbers in multiline mode?
    bool relative_line_numbers;                    // use relative line numbers when enabled?
    bool highlight_current_line_number;            // highlight the current line number differently?
    bool allow_line_numbers_with_continuation_prompt;  // keep line numbers when continuation
                                                       // prompts are active?
    bool replace_prompt_line_with_line_number;         // swap final prompt line with line numbers?
    bool show_whitespace_characters;                   // visualize spaces while editing?
    bool inline_right_prompt_follows_cursor;           // right prompt tracks cursor row
    bool bracketed_paste_enabled;                      // bracketed paste mode active
    bool readline_terminal_suspended;                  // external program owns the terminal
    bool suspended_mouse_reporting_enabled;            // restore mouse capture after resume
    bool suspended_focus_reporting_enabled;            // restore focus capture after resume
    bool typeahead_enabled;                            // capture pending stdin for next readline
    bool terminal_region_marking_enabled;              // emit OSC 133 semantic regions
    uint8_t terminal_region_state;                     // current OSC 133 lifecycle state
    size_t multiline_start_line_count;   // prefill multiline prompts with this many lines
    size_t multiline_max_line_count;     // maximum visible input rows in multiline mode
    size_t multiline_bottom_line_count;  // content-row margin around the cursor or menu selection
    size_t menu_max_line_count;          // maximum visible menu content rows
    long hint_delay;                     // delay before displaying a hint in milliseconds
    long idle_timeout;                   // inactivity timeout in milliseconds (0 disables)

    ic_key_binding_entry_t* key_bindings;  // dynamic array of custom key bindings
    ssize_t key_binding_count;
    ssize_t key_binding_capacity;
    const struct ic_keybinding_profile_s* key_binding_profile;

    ic_abbreviation_entry_t* abbreviations;
    ssize_t abbreviation_count;
    ssize_t abbreviation_capacity;

    ic_command_palette_entry_internal_t* command_palette_entries;
    ssize_t command_palette_entry_count;
    size_t command_palette_generation;
    ic_command_palette_entry_handler_t* command_palette_handler;
    void* command_palette_handler_arg;

    char* whitespace_marker;  // custom marker used when visualizing spaces
    char line_wrap_marker[5];        // one UTF-8 code point, or empty to hide soft-wrap indicators
    ssize_t line_wrap_marker_width;  // terminal columns reserved for the marker
};

ic_private char* ic_editline(ic_env_t* env, const char* prompt_text, const char* inline_right_text);

ic_private ic_env_t* ic_get_env(void);
ic_private ic_env_t* ic_get_env_if_initialized(void);
ic_private const char* ic_env_get_auto_braces(ic_env_t* env);
ic_private const char* ic_env_get_match_braces(ic_env_t* env);
ic_private void ic_env_set_initial_input(ic_env_t* env, const char* initial_input,
                                         size_t cursor_pos, bool cursor_pos_set);
ic_private void ic_env_clear_initial_input(ic_env_t* env);
ic_private const char* ic_env_get_whitespace_marker(ic_env_t* env);

#endif  // IC_ENV_H
