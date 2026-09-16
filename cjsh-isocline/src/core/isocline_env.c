/*
  isocline_env.c

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

/* ----------------------------------------------------------------------------
    Environment lifecycle management extracted from the original isocline.c.
-----------------------------------------------------------------------------*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "bbcode.h"
#include "common.h"
#include "completions.h"
#include "env.h"
#include "env_internal.h"
#include "history.h"
#include "isocline.h"
#include "keybinding_internal.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"
#include "unicode.h"

//-------------------------------------------------------------
// Prompt helpers shared with other modules
//-------------------------------------------------------------

static const char* ic_default_history_search_prompt = "history search: ";
static const char* ic_default_command_palette_prompt = "command palette: ";

ic_private bool ic_env_apply_line_wrap_marker(ic_env_t* env, const char* marker) {
    if (env == NULL) {
        return false;
    }
    if (marker == NULL) {
#ifdef __APPLE__
        marker = "\xE2\x86\xB5";  // return symbol
#else
        marker = "\xE2\x86\x90";  // left arrow
#endif
    }
    const ssize_t len = ic_strlen(marker);
    ssize_t width = 0;
    if (len > 0) {
        unicode_codepoint_t codepoint = 0;
        ssize_t bytes_read = 0;
        if (len > 4 || !unicode_decode_utf8((const uint8_t*)marker, len, &codepoint, &bytes_read) ||
            bytes_read != len || (width = unicode_codepoint_width(codepoint)) <= 0) {
            return false;
        }
    }
    // memmove also permits passing the getter's result back into the setter.
    memmove(env->line_wrap_marker, marker, (size_t)len + 1);
    env->line_wrap_marker_width = width;
    return true;
}

ic_private void ic_env_apply_prompt_markers(ic_env_t* env, const char* prompt_marker,
                                            const char* continuation_prompt_marker) {
    if (env == NULL) {
        return;
    }
    if (prompt_marker == NULL) {
        prompt_marker = "> ";
    }
    if (continuation_prompt_marker == NULL) {
        continuation_prompt_marker = prompt_marker;
    }
    mem_free(env->mem, env->prompt_marker);
    mem_free(env->mem, env->cprompt_marker);
    env->prompt_marker = mem_strdup(env->mem, prompt_marker);
    env->cprompt_marker = mem_strdup(env->mem, continuation_prompt_marker);
}

ic_private void ic_env_apply_history_search_prompt(ic_env_t* env, const char* prompt_text) {
    if (env == NULL) {
        return;
    }
    if (prompt_text == NULL) {
        prompt_text = ic_default_history_search_prompt;
    }
    mem_free(env->mem, env->history_search_prompt);
    env->history_search_prompt = mem_strdup(env->mem, prompt_text);
}

ic_private void ic_env_apply_command_palette_prompt(ic_env_t* env, const char* prompt_text) {
    if (env == NULL) {
        return;
    }
    if (prompt_text == NULL) {
        prompt_text = ic_default_command_palette_prompt;
    }
    mem_free(env->mem, env->command_palette_prompt);
    env->command_palette_prompt = mem_strdup(env->mem, prompt_text);
}

ic_private const char* ic_env_get_history_search_prompt(ic_env_t* env) {
    if (env == NULL || env->history_search_prompt == NULL) {
        return ic_default_history_search_prompt;
    }
    return env->history_search_prompt;
}

ic_private const char* ic_env_get_command_palette_prompt(ic_env_t* env) {
    if (env == NULL || env->command_palette_prompt == NULL) {
        return ic_default_command_palette_prompt;
    }
    return env->command_palette_prompt;
}

ic_private void ic_emit_continuation_indent(ic_env_t* env, const char* prompt_text) {
    if (env == NULL || env->no_multiline_indent || env->term == NULL || env->bbcode == NULL) {
        return;
    }
    const char* text = (prompt_text != NULL ? prompt_text : "");
    ssize_t textw = bbcode_column_width(env->bbcode, text);
    ssize_t markerw = bbcode_column_width(env->bbcode, env->prompt_marker);
    ssize_t cmarkerw = bbcode_column_width(env->bbcode, env->cprompt_marker);
    if (cmarkerw < markerw + textw) {
        term_write_repeat(env->term, " ", markerw + textw - cmarkerw);
    }
}

//-------------------------------------------------------------
// Environment allocation & teardown
//-------------------------------------------------------------

static void ic_atexit(void);

static ic_env_t* ic_env_create(ic_malloc_fun_t* _malloc, ic_realloc_fun_t* _realloc,
                               ic_free_fun_t* _free) {
    if (_malloc == NULL) {
        _malloc = &malloc;
    }
    if (_realloc == NULL) {
        _realloc = &realloc;
    }
    if (_free == NULL) {
        _free = &free;
    }
    // allocate allocator wrapper
    alloc_t* mem = (alloc_t*)_malloc(sizeof(alloc_t));
    if (mem == NULL) {
        return NULL;
    }
    mem->malloc = _malloc;
    mem->realloc = _realloc;
    mem->free = _free;

    ic_env_t* env = mem_zalloc_tp(mem, ic_env_t);
    if (env == NULL) {
        mem->free(mem);
        return NULL;
    }
    env->mem = mem;

    env->tty = tty_new(env->mem, -1);
    env->term = term_new(env->mem, env->tty, false, false, -1);
    // bracketed paste enabled when starting an interactive editor session
    env->history = history_new(env->mem);
    env->completions = completions_new(env->mem);
    env->bbcode = bbcode_new(env->mem, env->term);
    env->typeahead_input_buffer = sbuf_new(env->mem);
    env->typeahead_pending_raw_bytes = sbuf_new(env->mem);

    // Set default enabled features
    env->hint_delay = 0;                        // hint delay (0)
    env->idle_timeout = 0;                      // idle timeout disabled
    env->spell_correct = true;                  // completion spell fixing
    env->spell_correct_on_enter = false;        // enter-submit spell correction
    env->show_line_numbers = true;              // line numbers
    env->relative_line_numbers = false;         // absolute numbering by default
    env->highlight_current_line_number = true;  // highlight current line number by default
    env->allow_line_numbers_with_continuation_prompt = false;  // keep legacy suppression by default
    env->replace_prompt_line_with_line_number = false;  // keep final prompt line visible by default
    (void)ic_env_apply_line_wrap_marker(env, NULL);     // default soft-wrap indicator
    env->complete_nopreview = false;               // completion preview (inverted: false = enabled)
    env->completion_auto_menu = false;             // keep automatic completion menus opt-in
    env->completion_click_accept_enabled = false;  // keep click-to-accept off by default
    env->menu_highlight_mode = IC_MENU_HIGHLIGHT_NONE;  // keep menu items unhighlighted by default
    env->no_hint = false;                               // hint (inverted: false = enabled)
    env->complete_autotab = false;                      // auto tab (disabled by default)
    env->no_help = false;                               // inline help (inverted: false = enabled)
    env->no_multiline_indent = false;            // multiline indent (inverted: false = enabled)
    env->singleline_only = false;                // multiline (inverted: false = enabled)
    env->retain_multiline_continuation = false;  // remove "\\" when Enter continues by default
    env->multiline_start_line_count = 1;         // preallocated prompt lines when multiline is on
    env->multiline_max_line_count = 15;          // visible input rows before viewport scrolling
    env->multiline_bottom_line_count = 3;        // row margin around the cursor or menu selection
    env->menu_max_line_count = 50;               // visible menu content rows before scrolling
    env->last_readline_disposition = IC_READLINE_DISPOSITION_ERROR;
    env->status_hint_mode = IC_STATUS_HINT_NORMAL;  // default to legacy behavior
    env->mouse_reporting_mode =
        IC_MOUSE_CLICKING_MENU_ONLY;  // capture only inside menus by default
    env->history_search_sort = IC_HISTORY_SEARCH_SORT_RECENT;  // newest history entries first
    env->mouse_reporting_enabled_by_default = false;  // start editing sessions with capture off
    env->mouse_reporting_status_line_enabled = true;  // show indicator line when mouse is active
    env->inline_right_prompt_follows_cursor = false;  // keep right prompt anchored at row 0
    env->bracketed_paste_enabled = false;
    env->readline_terminal_suspended = false;
    env->suspended_mouse_reporting_enabled = false;
    env->suspended_focus_reporting_enabled = false;
    env->typeahead_enabled = false;                // callers opt in for interactive shell sessions
    env->terminal_region_marking_enabled = false;  // callers opt in to OSC 133 shell integration
    env->terminal_region_state = 0;

    if (env->tty == NULL || env->term == NULL || env->completions == NULL || env->history == NULL ||
        env->bbcode == NULL || env->typeahead_input_buffer == NULL ||
        env->typeahead_pending_raw_bytes == NULL || !term_is_interactive(env->term)) {
        env->noedit = true;
    }
    env->multiline_eol = '\\';

    bbcode_style_def(env->bbcode, "ic-prompt", "ansi-white");
    bbcode_style_def(env->bbcode, "ic-linenumbers", "ansi-lightgray");
    bbcode_style_def(env->bbcode, "ic-linenumber-current", "ansi-yellow");
    bbcode_style_def(env->bbcode, "ic-info", "ansi-darkgray");
    bbcode_style_def(env->bbcode, "ic-status", "ansi-lightgray");
    bbcode_style_def(env->bbcode, "ic-source", "#ffffd7");
    bbcode_style_def(env->bbcode, "ic-diminish", "ansi-lightgray");
    bbcode_style_def(env->bbcode, "ic-emphasis", "#ffffd7");
    bbcode_style_def(env->bbcode, "ic-menu-selected", "ansi-black on #ffffd7");
    bbcode_style_def(env->bbcode, "ic-menu-selected-secondary", "ansi-darkgray on #ffffd7");
    bbcode_style_def(env->bbcode, "ic-hint", "ansi-darkgray");
    bbcode_style_def(env->bbcode, "ic-error", "#d70000");
    bbcode_style_def(env->bbcode, "ic-bracematch", "ansi-white");
    bbcode_style_def(env->bbcode, "ic-whitespace-char", "ansi-lightgray");

    bbcode_style_def(env->bbcode, "keyword", "#569cd6");
    bbcode_style_def(env->bbcode, "control", "#c586c0");
    bbcode_style_def(env->bbcode, "number", "#b5cea8");
    bbcode_style_def(env->bbcode, "string", "#ce9178");
    bbcode_style_def(env->bbcode, "comment", "#6A9955");
    bbcode_style_def(env->bbcode, "type", "darkcyan");
    bbcode_style_def(env->bbcode, "constant", "#569cd6");

    ic_env_apply_prompt_markers(env, NULL, NULL);
    ic_env_apply_history_search_prompt(env, NULL);
    ic_env_apply_command_palette_prompt(env, NULL);
    env->key_binding_profile = ic_keybinding_profile_default_ptr();

    return env;
}

static void ic_env_free(ic_env_t* env) {
    if (env == NULL) {
        return;
    }
    if (env->bracketed_paste_enabled && env->term != NULL && term_is_interactive(env->term)) {
        term_write(env->term, "\x1b[?2004l");
    }
    history_free(env->history);
    completions_free(env->completions);
    bbcode_free(env->bbcode);
    sbuf_free(env->typeahead_input_buffer);
    sbuf_free(env->typeahead_pending_raw_bytes);
    sbuf_free(env->notifications);
    term_free(env->term);
    tty_free(env->tty);
    if (env->abbreviations != NULL) {
        for (ssize_t i = 0; i < env->abbreviation_count; ++i) {
            mem_free(env->mem, env->abbreviations[i].trigger);
            mem_free(env->mem, env->abbreviations[i].expansion);
        }
        mem_free(env->mem, env->abbreviations);
        env->abbreviations = NULL;
        env->abbreviation_count = 0;
        env->abbreviation_capacity = 0;
    }
    if (env->command_palette_entries != NULL) {
        for (ssize_t i = 0; i < env->command_palette_entry_count; ++i) {
            mem_free(env->mem, env->command_palette_entries[i].id);
            mem_free(env->mem, env->command_palette_entries[i].name);
            mem_free(env->mem, env->command_palette_entries[i].description);
            mem_free(env->mem, env->command_palette_entries[i].keywords);
        }
        mem_free(env->mem, env->command_palette_entries);
        env->command_palette_entries = NULL;
        env->command_palette_entry_count = 0;
    }
    mem_free(env->mem, env->cprompt_marker);
    mem_free(env->mem, env->prompt_marker);
    mem_free(env->mem, env->prompt_eol_mark);
    mem_free(env->mem, env->history_search_prompt);
    mem_free(env->mem, env->command_palette_prompt);
    mem_free(env->mem, env->history_search_sort_key);
    mem_free(env->mem, env->match_braces);
    mem_free(env->mem, env->auto_braces);
    mem_free(env->mem, (void*)env->initial_input);
    mem_free(env->mem, env->key_bindings);
    mem_free(env->mem, env->whitespace_marker);
    env->prompt_marker = NULL;

    alloc_t* mem = env->mem;
    mem_free(mem, env);
    mem_free(mem, mem);
}

//-------------------------------------------------------------
// Global environment accessors
//-------------------------------------------------------------

static ic_env_t* rpenv = NULL;
static bool ic_default_abbreviations_initialized = false;

ic_private ic_env_t* ic_get_env_if_initialized(void) {
    return rpenv;
}

static void ic_atexit(void) {
    if (rpenv != NULL) {
        ic_env_free(rpenv);
        rpenv = NULL;
    }
}

ic_private ic_env_t* ic_get_env(void) {
    if (rpenv == NULL) {
        rpenv = ic_env_create(NULL, NULL, NULL);
        if (rpenv != NULL) {
            (void)atexit(&ic_atexit);
        }
    }
    if (!ic_default_abbreviations_initialized && rpenv != NULL) {
        ic_default_abbreviations_initialized = true;
        (void)ic_add_abbreviation("abbr", "abbreviate");
        (void)ic_add_abbreviation("unabbr", "unabbreviate");
    }
    return rpenv;
}

ic_public void ic_init_custom_malloc(ic_malloc_fun_t* _malloc, ic_realloc_fun_t* _realloc,
                                     ic_free_fun_t* _free) {
    assert(rpenv == NULL);
    if (rpenv != NULL) {
        ic_env_free(rpenv);
        rpenv = ic_env_create(_malloc, _realloc, _free);
    } else {
        rpenv = ic_env_create(_malloc, _realloc, _free);
        if (rpenv != NULL) {
            (void)atexit(&ic_atexit);
        }
    }
}

ic_private const char* ic_env_get_match_braces(ic_env_t* env) {
    return (env->match_braces == NULL ? "()[]{}" : env->match_braces);
}

ic_private const char* ic_env_get_auto_braces(ic_env_t* env) {
    return (env->auto_braces == NULL ? "()[]{}\"\"''" : env->auto_braces);
}

ic_private const char* ic_env_get_whitespace_marker(ic_env_t* env) {
    static const char* default_marker = "\xC2\xB7";  // middle dot indicator
    if (env == NULL || env->whitespace_marker == NULL || env->whitespace_marker[0] == '\0') {
        return default_marker;
    }
    return env->whitespace_marker;
}

ic_private void ic_env_set_initial_input(ic_env_t* env, const char* initial_input,
                                         size_t cursor_pos, bool cursor_pos_set) {
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, (void*)env->initial_input);
    env->initial_input = NULL;
    if (initial_input != NULL) {
        env->initial_input = mem_strdup(env->mem, initial_input);
    }
    env->initial_cursor_pos = cursor_pos;
    env->initial_cursor_pos_set = cursor_pos_set;
}

ic_private void ic_env_clear_initial_input(ic_env_t* env) {
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, (void*)env->initial_input);
    env->initial_input = NULL;
    env->initial_cursor_pos = 0;
    env->initial_cursor_pos_set = false;
}
