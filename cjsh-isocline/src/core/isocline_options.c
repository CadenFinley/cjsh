/*
  isocline_options.c

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
    Runtime configuration helpers split from the original isocline.c file.
-----------------------------------------------------------------------------*/

#include "common.h"
#include "env.h"
#include "env_internal.h"
#include "history.h"
#include "isocline.h"
#include "term.h"
#include "tty.h"

#include <ctype.h>
#include <string.h>
#include <sys/types.h>

static ic_abbreviation_entry_t* ic_env_find_abbreviation(ic_env_t* env, const char* trigger,
                                                         ssize_t trigger_len, ssize_t* out_index) {
    if (env == NULL || trigger == NULL || trigger_len <= 0) {
        return NULL;
    }
    for (ssize_t i = 0; i < env->abbreviation_count; ++i) {
        ic_abbreviation_entry_t* entry = &env->abbreviations[i];
        if (entry->trigger_len == trigger_len &&
            strncmp(entry->trigger, trigger, (size_t)trigger_len) == 0) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return entry;
        }
    }
    return NULL;
}

static bool ic_env_ensure_abbreviation_capacity(ic_env_t* env, ssize_t needed) {
    if (env->abbreviation_capacity >= needed) {
        return true;
    }
    ssize_t new_capacity = (env->abbreviation_capacity == 0 ? 4 : env->abbreviation_capacity * 2);
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ic_abbreviation_entry_t* resized =
        mem_realloc_tp(env->mem, ic_abbreviation_entry_t, env->abbreviations, new_capacity);
    if (resized == NULL) {
        return false;
    }
    env->abbreviations = resized;
    env->abbreviation_capacity = new_capacity;
    return true;
}

static void ic_env_clear_command_palette_entries(ic_env_t* env) {
    if (env != NULL) {
        ++env->command_palette_generation;
    }
    if (env == NULL || env->command_palette_entries == NULL) {
        if (env != NULL) {
            env->command_palette_entry_count = 0;
        }
        return;
    }

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

static bool ic_history_search_sort_key_valid(const char* key) {
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    for (const char* p = key; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) || c == '=') {
            return false;
        }
    }
    return true;
}

ic_public const char* ic_get_prompt_marker(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return env->prompt_marker;
}

ic_public const char* ic_get_continuation_prompt_marker(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return env->cprompt_marker;
}

ic_public void ic_set_prompt_eol_mark(const char* eol_mark) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, env->prompt_eol_mark);
    env->prompt_eol_mark = NULL;
    if (eol_mark != NULL) {
        env->prompt_eol_mark = mem_strdup(env->mem, eol_mark);
    }
}

ic_public const char* ic_get_prompt_eol_mark(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return env->prompt_eol_mark;
}

ic_public void ic_set_prompt_marker(const char* prompt_marker, const char* cprompt_marker) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    ic_env_apply_prompt_markers(env, prompt_marker, cprompt_marker);
}

ic_public void ic_set_history_search_prompt(const char* prompt_text) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    ic_env_apply_history_search_prompt(env, prompt_text);
}

ic_public const char* ic_get_history_search_prompt(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return ic_env_get_history_search_prompt(env);
}

ic_public void ic_set_command_palette_prompt(const char* prompt_text) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    ic_env_apply_command_palette_prompt(env, prompt_text);
}

ic_public const char* ic_get_command_palette_prompt(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return ic_env_get_command_palette_prompt(env);
}

ic_public bool ic_enable_multiline(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->singleline_only;
    env->singleline_only = !enable;
    return !prev;
}

ic_public bool ic_multiline_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && !env->singleline_only;
}

ic_public bool ic_enable_multiline_continuation_retention(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->retain_multiline_continuation;
    env->retain_multiline_continuation = enable;
    return prev;
}

ic_public bool ic_enable_beep(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    if (env->term == NULL) {
        return false;
    }
    return term_enable_beep(env->term, enable);
}

ic_public bool ic_enable_color(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    if (env->term == NULL) {
        return false;
    }
    return term_enable_color(env->term, enable);
}

ic_public bool ic_enable_history_auto_add(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return history_enable_auto_add(env->history, enable);
}

ic_public bool ic_enable_history_duplicates(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return history_enable_duplicates(env->history, enable);
}

ic_public bool ic_enable_history_fuzzy_case_sensitive(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return true;
    }
    return history_set_fuzzy_case_sensitive(env->history, enable);
}

ic_public bool ic_history_fuzzy_search_is_case_sensitive(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return true;
    }
    return history_is_fuzzy_case_sensitive(env->history);
}

ic_public bool ic_enable_history_directory(bool enable) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_enable_directory(env->history, enable);
}

ic_public bool ic_history_directory_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_directory_is_enabled(env->history);
}

ic_public bool ic_enable_history_directory_subdirs(bool enable) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_enable_directory_subdirs(env->history, enable);
}

ic_public bool ic_history_directory_subdirs_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_directory_subdirs_is_enabled(env->history);
}

ic_public bool ic_set_history_directory(const char* directory) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_set_directory(env->history, directory);
}

ic_public bool ic_history_matches_directory(const char* directory) {
    ic_env_t* env = ic_get_env();
    return env != NULL && history_matches_directory(env->history, directory);
}

ic_public bool ic_set_history_search_sort(ic_history_search_sort_t sort, const char* metadata_key) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }

    bool needs_metadata_key = false;
    switch (sort) {
        case IC_HISTORY_SEARCH_SORT_RECENT:
        case IC_HISTORY_SEARCH_SORT_COMMAND_ASC:
        case IC_HISTORY_SEARCH_SORT_COMMAND_DESC:
            break;
        case IC_HISTORY_SEARCH_SORT_METADATA_ASC:
        case IC_HISTORY_SEARCH_SORT_METADATA_DESC:
            needs_metadata_key = true;
            break;
        default:
            return false;
    }

    char* key_copy = NULL;
    if (needs_metadata_key) {
        if (!ic_history_search_sort_key_valid(metadata_key)) {
            return false;
        }
        key_copy = mem_strdup(env->mem, metadata_key);
        if (key_copy == NULL) {
            return false;
        }
    }

    mem_free(env->mem, env->history_search_sort_key);
    env->history_search_sort_key = key_copy;
    env->history_search_sort = sort;
    return true;
}

ic_public ic_history_search_sort_t ic_get_history_search_sort(const char** metadata_key) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        if (metadata_key != NULL) {
            *metadata_key = NULL;
        }
        return IC_HISTORY_SEARCH_SORT_RECENT;
    }

    if (metadata_key != NULL) {
        switch (env->history_search_sort) {
            case IC_HISTORY_SEARCH_SORT_METADATA_ASC:
            case IC_HISTORY_SEARCH_SORT_METADATA_DESC:
                *metadata_key = env->history_search_sort_key;
                break;
            default:
                *metadata_key = NULL;
                break;
        }
    }
    return env->history_search_sort;
}

ic_public void ic_set_history(const char* fname, long max_entries) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    history_load_from(env->history, fname, max_entries);
}

ic_public void ic_history_remove_last(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    history_remove_last(env->history);
}

ic_public void ic_history_add_with_metadata(const char* entry,
                                            const ic_history_metadata_t* metadata,
                                            size_t metadata_count) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    (void)history_push_with_metadata(env->history, entry, metadata, metadata_count);
}

ic_public void ic_history_add(const char* entry) {
    ic_history_add_with_metadata(entry, NULL, 0);
}

ic_public void ic_history_clear(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    history_clear(env->history);
}

ic_public bool ic_enable_auto_tab(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->complete_autotab;
    env->complete_autotab = enable;
    return prev;
}

ic_public bool ic_auto_tab_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && env->complete_autotab;
}

ic_public bool ic_enable_completion_preview(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->complete_nopreview;
    env->complete_nopreview = !enable;
    return !prev;
}

ic_public bool ic_completion_preview_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && !env->complete_nopreview;
}

ic_public bool ic_enable_completion_auto_menu(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    const bool prev = env->completion_auto_menu;
    env->completion_auto_menu = enable;
    return prev;
}

ic_public bool ic_completion_auto_menu_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    return env != NULL && env->completion_auto_menu;
}

ic_public bool ic_enable_completion_click_accept(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }

    bool prev = env->completion_click_accept_enabled;
    env->completion_click_accept_enabled = enable;
    return prev;
}

ic_public bool ic_completion_click_accept_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->completion_click_accept_enabled;
}

ic_public ic_menu_highlight_mode_t ic_set_menu_highlight_mode(ic_menu_highlight_mode_t mode) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_MENU_HIGHLIGHT_NONE;
    }

    ic_menu_highlight_mode_t prev = env->menu_highlight_mode;
    switch (mode) {
        case IC_MENU_HIGHLIGHT_NONE:
        case IC_MENU_HIGHLIGHT_SINGLE:
        case IC_MENU_HIGHLIGHT_ALL:
        case IC_MENU_HIGHLIGHT_REVERSE:
            env->menu_highlight_mode = mode;
            break;
        default:
            env->menu_highlight_mode = IC_MENU_HIGHLIGHT_NONE;
            break;
    }
    return prev;
}

ic_public ic_menu_highlight_mode_t ic_get_menu_highlight_mode(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_MENU_HIGHLIGHT_NONE;
    }
    return env->menu_highlight_mode;
}

ic_public bool ic_enable_multiline_indent(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_multiline_indent;
    env->no_multiline_indent = !enable;
    return !prev;
}

ic_public bool ic_multiline_indent_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && !env->no_multiline_indent;
}

ic_public size_t ic_set_multiline_start_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 1;
    }

    size_t prev = env->multiline_start_line_count;
    if (line_count < 1) {
        line_count = 1;
    }

    const size_t max_lines = 256;  // prevent pathological allocations
    if (line_count > max_lines) {
        line_count = max_lines;
    }

    env->multiline_start_line_count = line_count;
    return prev;
}

ic_public size_t ic_get_multiline_start_line_count(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 1;
    }
    return env->multiline_start_line_count;
}

ic_public size_t ic_set_multiline_max_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 15;
    }

    size_t prev = env->multiline_max_line_count;
    if (line_count < 1) {
        line_count = 1;
    }

    const size_t max_lines = 256;
    if (line_count > max_lines) {
        line_count = max_lines;
    }

    env->multiline_max_line_count = line_count;
    return prev;
}

ic_public size_t ic_get_multiline_max_line_count(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 15;
    }
    return env->multiline_max_line_count;
}

ic_public size_t ic_set_multiline_bottom_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 3;
    }

    size_t prev = env->multiline_bottom_line_count;
    const size_t max_lines = 256;
    if (line_count > max_lines) {
        line_count = max_lines;
    }

    env->multiline_bottom_line_count = line_count;
    return prev;
}

ic_public size_t ic_get_multiline_bottom_line_count(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 3;
    }
    return env->multiline_bottom_line_count;
}

static size_t ic_set_menu_line_limit(size_t* limit, size_t line_count, size_t default_limit) {
    if (limit == NULL) {
        return default_limit;
    }
    const size_t prev = *limit;
    if (line_count < 1) {
        line_count = 1;
    } else if (line_count > 256) {
        line_count = 256;
    }
    *limit = line_count;
    return prev;
}

ic_public size_t ic_set_completion_menu_max_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    return ic_set_menu_line_limit(env == NULL ? NULL : &env->completion_menu_max_line_count,
                                  line_count, 15);
}

ic_public size_t ic_get_completion_menu_max_line_count(void) {
    ic_env_t* env = ic_get_env();
    return (env == NULL ? 15 : env->completion_menu_max_line_count);
}

ic_public size_t ic_set_history_menu_max_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    return ic_set_menu_line_limit(env == NULL ? NULL : &env->history_menu_max_line_count,
                                  line_count, 30);
}

ic_public size_t ic_get_history_menu_max_line_count(void) {
    ic_env_t* env = ic_get_env();
    return (env == NULL ? 30 : env->history_menu_max_line_count);
}

ic_public size_t ic_set_command_palette_max_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    return ic_set_menu_line_limit(env == NULL ? NULL : &env->command_palette_max_line_count,
                                  line_count, 30);
}

ic_public size_t ic_get_command_palette_max_line_count(void) {
    ic_env_t* env = ic_get_env();
    return (env == NULL ? 30 : env->command_palette_max_line_count);
}

ic_public size_t ic_set_custom_menu_max_line_count(size_t line_count) {
    ic_env_t* env = ic_get_env();
    return ic_set_menu_line_limit(env == NULL ? NULL : &env->custom_menu_max_line_count,
                                  line_count, 30);
}

ic_public size_t ic_get_custom_menu_max_line_count(void) {
    ic_env_t* env = ic_get_env();
    return (env == NULL ? 30 : env->custom_menu_max_line_count);
}

ic_public bool ic_enable_line_numbers(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->show_line_numbers;
    env->show_line_numbers = enable;
    if (!enable) {
        env->relative_line_numbers = false;
    }
    return prev;
}

ic_public bool ic_enable_relative_line_numbers(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->relative_line_numbers;
    env->relative_line_numbers = enable;
    if (enable) {
        env->show_line_numbers = true;
    }
    return prev;
}

ic_public bool ic_line_numbers_are_enabled(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->show_line_numbers;
}

ic_public bool ic_line_numbers_are_relative(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->relative_line_numbers;
}

ic_public bool ic_enable_line_numbers_with_continuation_prompt(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->allow_line_numbers_with_continuation_prompt;
    env->allow_line_numbers_with_continuation_prompt = enable;
    return prev;
}

ic_public bool ic_line_numbers_with_continuation_prompt_are_enabled(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->allow_line_numbers_with_continuation_prompt;
}

ic_public bool ic_enable_line_number_prompt_replacement(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->replace_prompt_line_with_line_number;
    env->replace_prompt_line_with_line_number = enable;
    return prev;
}

ic_public bool ic_line_number_prompt_replacement_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->replace_prompt_line_with_line_number;
}

ic_public bool ic_enable_current_line_number_highlight(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->highlight_current_line_number;
    env->highlight_current_line_number = enable;
    return prev;
}

ic_public bool ic_current_line_number_highlight_is_enabled(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->highlight_current_line_number;
}

ic_public bool ic_set_line_wrap_marker(const char* marker) {
    return ic_env_apply_line_wrap_marker(ic_get_env(), marker);
}

ic_public const char* ic_get_line_wrap_marker(void) {
    const ic_env_t* env = ic_get_env();
    return (env == NULL ? NULL : env->line_wrap_marker);
}

ic_public bool ic_enable_visible_whitespace(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->show_whitespace_characters;
    env->show_whitespace_characters = enable;
    return prev;
}

ic_public bool ic_visible_whitespace_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && env->show_whitespace_characters;
}

ic_public void ic_set_whitespace_marker(const char* marker) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, env->whitespace_marker);
    env->whitespace_marker = NULL;
    if (marker != NULL && marker[0] != '\0') {
        env->whitespace_marker = mem_strdup(env->mem, marker);
    }
}

ic_public const char* ic_get_whitespace_marker(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return ic_env_get_whitespace_marker(env);
}

ic_public bool ic_enable_hint(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_hint;
    env->no_hint = !enable;
    return !prev;
}

ic_public bool ic_hint_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && !env->no_hint;
}

ic_public bool ic_enable_spell_correct(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->spell_correct;
    env->spell_correct = enable;
    return prev;
}

ic_public bool ic_enable_spell_correct_on_enter(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->spell_correct_on_enter;
    env->spell_correct_on_enter = enable;
    return prev;
}

ic_public long ic_set_hint_delay(long delay_ms) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 0;
    }
    long prev = env->hint_delay;
    if (delay_ms < 0) {
        env->hint_delay = 0;
    } else if (delay_ms > 5000) {
        env->hint_delay = 5000;
    } else {
        env->hint_delay = delay_ms;
    }
    return prev;
}

ic_public long ic_get_hint_delay(void) {
    ic_env_t* env = ic_get_env();
    return (env == NULL ? 0 : env->hint_delay);
}

ic_public long ic_set_idle_timeout(long timeout_ms) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 0;
    }
    long prev = env->idle_timeout;
    env->idle_timeout = (timeout_ms < 0 ? 0 : timeout_ms);
    return prev;
}

ic_public void ic_set_tty_esc_delay(long initial_delay_ms, long followup_delay_ms) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    if (env->tty == NULL) {
        return;
    }
    tty_set_esc_delay(env->tty, initial_delay_ms, followup_delay_ms);
}

ic_public bool ic_enable_highlight(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_highlight;
    env->no_highlight = !enable;
    return !prev;
}

ic_public bool ic_enable_inline_help(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_help;
    env->no_help = !enable;
    return !prev;
}

ic_public bool ic_inline_help_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && !env->no_help;
}

ic_public ic_status_hint_mode_t ic_set_status_hint_mode(ic_status_hint_mode_t mode) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_STATUS_HINT_NORMAL;
    }

    ic_status_hint_mode_t prev = env->status_hint_mode;
    switch (mode) {
        case IC_STATUS_HINT_OFF:
        case IC_STATUS_HINT_NORMAL:
        case IC_STATUS_HINT_TRANSIENT:
        case IC_STATUS_HINT_PERSISTENT:
            env->status_hint_mode = mode;
            break;
        default:
            env->status_hint_mode = IC_STATUS_HINT_NORMAL;
            break;
    }
    return prev;
}

ic_public ic_status_hint_mode_t ic_get_status_hint_mode(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_STATUS_HINT_NORMAL;
    }
    return env->status_hint_mode;
}

static ic_mouse_clicking_mode_t ic_normalize_mouse_clicking_mode(ic_mouse_clicking_mode_t mode) {
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

ic_public ic_mouse_clicking_mode_t ic_set_mouse_clicking_mode(ic_mouse_clicking_mode_t mode) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_MOUSE_CLICKING_DISABLED;
    }

    ic_mouse_clicking_mode_t prev = env->mouse_reporting_mode;
    env->mouse_reporting_mode = ic_normalize_mouse_clicking_mode(mode);
    env->mouse_reporting_enabled_by_default =
        (env->mouse_reporting_mode == IC_MOUSE_CLICKING_SIMPLE ||
         env->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART);
    return prev;
}

ic_public ic_mouse_clicking_mode_t ic_get_mouse_clicking_mode(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return IC_MOUSE_CLICKING_DISABLED;
    }
    return ic_normalize_mouse_clicking_mode(env->mouse_reporting_mode);
}

ic_public bool ic_enable_mouse_clicking(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->mouse_reporting_enabled_by_default;
    env->mouse_reporting_enabled_by_default = enable;
    if (enable && (env->mouse_reporting_mode == IC_MOUSE_CLICKING_DISABLED ||
                   env->mouse_reporting_mode == IC_MOUSE_CLICKING_MENU_ONLY)) {
        env->mouse_reporting_mode = IC_MOUSE_CLICKING_SIMPLE;
    }
    return prev;
}

ic_public bool ic_enable_mouse_reporting_status_line(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->mouse_reporting_status_line_enabled;
    env->mouse_reporting_status_line_enabled = enable;
    return prev;
}

ic_public bool ic_mouse_reporting_status_line_is_enabled(void) {
    const ic_env_t* env = ic_get_env();
    return env != NULL && env->mouse_reporting_status_line_enabled;
}

ic_public bool ic_enable_inline_right_prompt_cursor_follow(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->inline_right_prompt_follows_cursor;
    env->inline_right_prompt_follows_cursor = enable;
    return prev;
}

ic_public bool ic_inline_right_prompt_follows_cursor(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    return env->inline_right_prompt_follows_cursor;
}

ic_public bool ic_enable_brace_matching(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_bracematch;
    env->no_bracematch = !enable;
    return !prev;
}

ic_public void ic_set_matching_braces(const char* brace_pairs) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, env->match_braces);
    env->match_braces = NULL;
    if (brace_pairs != NULL) {
        ssize_t len = ic_strlen(brace_pairs);
        if (len > 0 && (len % 2) == 0) {
            env->match_braces = mem_strdup(env->mem, brace_pairs);
        }
    }
}

ic_public bool ic_enable_brace_insertion(bool enable) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    bool prev = env->no_autobrace;
    env->no_autobrace = !enable;
    return !prev;
}

ic_public void ic_set_insertion_braces(const char* brace_pairs) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, env->auto_braces);
    env->auto_braces = NULL;
    if (brace_pairs != NULL) {
        ssize_t len = ic_strlen(brace_pairs);
        if (len > 0 && (len % 2) == 0) {
            env->auto_braces = mem_strdup(env->mem, brace_pairs);
        }
    }
}

ic_public bool ic_add_abbreviation(const char* trigger, const char* expansion) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || trigger == NULL || expansion == NULL) {
        return false;
    }

    ssize_t trigger_len = ic_strlen(trigger);
    if (trigger_len <= 0) {
        return false;
    }

    for (ssize_t i = 0; i < trigger_len; ++i) {
        if (ic_char_is_white(trigger + i, 1)) {
            return false;
        }
    }

    ic_abbreviation_entry_t* existing = ic_env_find_abbreviation(env, trigger, trigger_len, NULL);
    if (existing != NULL) {
        char* expansion_copy = mem_strdup(env->mem, expansion);
        if (expansion_copy == NULL) {
            return false;
        }
        mem_free(env->mem, existing->expansion);
        existing->expansion = expansion_copy;
        return true;
    }

    if (!ic_env_ensure_abbreviation_capacity(env, env->abbreviation_count + 1)) {
        return false;
    }

    char* trigger_copy = mem_strdup(env->mem, trigger);
    if (trigger_copy == NULL) {
        return false;
    }

    char* expansion_copy = mem_strdup(env->mem, expansion);
    if (expansion_copy == NULL) {
        mem_free(env->mem, trigger_copy);
        return false;
    }

    ic_abbreviation_entry_t* entry = &env->abbreviations[env->abbreviation_count];
    entry->trigger = trigger_copy;
    entry->expansion = expansion_copy;
    entry->trigger_len = trigger_len;
    env->abbreviation_count += 1;
    return true;
}

ic_public bool ic_remove_abbreviation(const char* trigger) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || trigger == NULL || env->abbreviation_count <= 0) {
        return false;
    }

    ssize_t trigger_len = ic_strlen(trigger);
    if (trigger_len <= 0) {
        return false;
    }

    ssize_t index = -1;
    ic_abbreviation_entry_t* entry = ic_env_find_abbreviation(env, trigger, trigger_len, &index);
    if (entry == NULL) {
        return false;
    }

    mem_free(env->mem, entry->trigger);
    mem_free(env->mem, entry->expansion);

    ssize_t remaining = env->abbreviation_count - index - 1;
    if (remaining > 0) {
        ic_memmove(&env->abbreviations[index], &env->abbreviations[index + 1],
                   remaining * ssizeof(ic_abbreviation_entry_t));
    }

    env->abbreviation_count -= 1;
    return true;
}

ic_public void ic_clear_abbreviations(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->abbreviation_count <= 0 || env->abbreviations == NULL) {
        return;
    }

    for (ssize_t i = 0; i < env->abbreviation_count; ++i) {
        mem_free(env->mem, env->abbreviations[i].trigger);
        mem_free(env->mem, env->abbreviations[i].expansion);
    }

    mem_free(env->mem, env->abbreviations);
    env->abbreviations = NULL;
    env->abbreviation_count = 0;
    env->abbreviation_capacity = 0;
}

ic_public bool ic_set_command_palette_entries(const ic_command_palette_entry_t* entries,
                                              size_t count) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }

    if (count == 0) {
        ic_env_clear_command_palette_entries(env);
        return true;
    }

    if (entries == NULL) {
        return false;
    }

    const ssize_t ssize_count = to_ssize_t(count);
    if (ssize_count <= 0) {
        return false;
    }

    ic_command_palette_entry_internal_t* copied =
        mem_zalloc_tp_n(env->mem, ic_command_palette_entry_internal_t, ssize_count);
    if (copied == NULL) {
        return false;
    }

    for (ssize_t i = 0; i < ssize_count; ++i) {
        const ic_command_palette_entry_t* source = &entries[i];
        const char* source_name = source->name;
        if (source_name == NULL || source_name[0] == '\0') {
            for (ssize_t j = 0; j <= i; ++j) {
                mem_free(env->mem, copied[j].id);
                mem_free(env->mem, copied[j].name);
                mem_free(env->mem, copied[j].description);
                mem_free(env->mem, copied[j].keywords);
            }
            mem_free(env->mem, copied);
            return false;
        }

        const char* source_id = source->id;
        if (source_id == NULL || source_id[0] == '\0') {
            source_id = source_name;
        }
        const char* source_description = (source->description != NULL ? source->description : "");
        const char* source_keywords = (source->keywords != NULL ? source->keywords : "");

        copied[i].id = mem_strdup(env->mem, source_id);
        copied[i].name = mem_strdup(env->mem, source_name);
        copied[i].description = mem_strdup(env->mem, source_description);
        copied[i].keywords = mem_strdup(env->mem, source_keywords);

        if (copied[i].id == NULL || copied[i].name == NULL || copied[i].description == NULL ||
            copied[i].keywords == NULL) {
            for (ssize_t j = 0; j <= i; ++j) {
                mem_free(env->mem, copied[j].id);
                mem_free(env->mem, copied[j].name);
                mem_free(env->mem, copied[j].description);
                mem_free(env->mem, copied[j].keywords);
            }
            mem_free(env->mem, copied);
            return false;
        }
    }

    ic_env_clear_command_palette_entries(env);
    env->command_palette_entries = copied;
    env->command_palette_entry_count = ssize_count;
    return true;
}

ic_public void ic_clear_command_palette_entries(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    ic_env_clear_command_palette_entries(env);
}

ic_public size_t ic_list_command_palette_entries(ic_command_palette_entry_t* buffer,
                                                 size_t capacity) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->command_palette_entry_count <= 0 ||
        env->command_palette_entries == NULL) {
        return 0;
    }

    size_t count = to_size_t(env->command_palette_entry_count);
    if (buffer == NULL || capacity == 0) {
        return count;
    }

    size_t limit = (count < capacity ? count : capacity);
    for (size_t i = 0; i < limit; ++i) {
        const ic_command_palette_entry_internal_t* src = &env->command_palette_entries[i];
        buffer[i].id = src->id;
        buffer[i].name = src->name;
        buffer[i].description = src->description;
        buffer[i].keywords = src->keywords;
    }
    return limit;
}

ic_public void ic_set_command_palette_entry_handler(ic_command_palette_entry_handler_t* handler,
                                                    void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    env->command_palette_handler = handler;
    env->command_palette_handler_arg = arg;
}

ic_public void ic_set_default_highlighter(ic_highlight_fun_t* highlighter, void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    env->highlighter = highlighter;
    env->highlighter_arg = arg;
}

ic_public void ic_set_unhandled_key_handler(ic_unhandled_key_fun_t* callback, void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    env->unhandled_key_handler = callback;
    env->unhandled_key_arg = arg;
}

ic_public void ic_set_status_message_callback(ic_status_message_fun_t* callback, void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    env->status_message_callback = callback;
    env->status_message_arg = arg;
}

ic_public void ic_set_check_for_continuation_or_return_callback(
    ic_check_for_continuation_or_return_fun_t* callback, void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    env->continuation_check_callback = callback;
    env->continuation_check_arg = arg;
}

ic_public void ic_free(void* p) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    mem_free(env->mem, p);
}

ic_public void* ic_malloc(size_t sz) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    return mem_malloc(env->mem, to_ssize_t(sz));
}

ic_public const char* ic_strdup(const char* s) {
    if (s == NULL) {
        return NULL;
    }
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return NULL;
    }
    ssize_t len = ic_strlen(s);
    char* p = mem_malloc_tp_n(env->mem, char, len + 1);
    if (p == NULL) {
        return NULL;
    }
    ic_memcpy(p, s, len);
    p[len] = '\0';
    return p;
}
