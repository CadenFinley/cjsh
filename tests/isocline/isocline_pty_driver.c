/*
  isocline_pty_driver.c

  This file is part of cjsh, CJ's Shell

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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "keybindings.h"
#include "keycodes.h"

#if !defined(_WIN32)
#include <errno.h>
#include <sys/select.h>  // IWYU pragma: keep
#include <sys/time.h>    // IWYU pragma: keep
#include <unistd.h>
#endif

#include "env.h"
#include "history.h"
#include "isocline.h"
#include "isocline_typeahead.h"
#include "tty.h"

typedef enum completion_mode_e {
    COMPLETION_MODE_NONE = 0,
    COMPLETION_MODE_SINGLE,
    COMPLETION_MODE_DUAL,
    COMPLETION_MODE_MANY,
    COMPLETION_MODE_MANY_MULTILINE,
    COMPLETION_MODE_MANY_MULTILINE_REPLACEMENT,
    COMPLETION_MODE_MANY_TALL_REPLACEMENT,
    COMPLETION_MODE_SPELL_SINGLE,
    COMPLETION_MODE_SPELL_MIXED,
    COMPLETION_MODE_SPELL_CROSS_TOKEN,
} completion_mode_t;

static completion_mode_t g_completion_mode = COMPLETION_MODE_NONE;
static bool g_notify_from_completion = false;
static bool g_flatten_completion_display = false;
static bool g_wrap_completion_input = false;

typedef struct paste_status_s {
    bool saw_complete;
    bool saw_partial;
} paste_status_t;

static const char* paste_status_callback(const char* input, void* arg) {
    paste_status_t* state = (paste_status_t*)arg;
    if (input[0] == '\0') {
        return NULL;
    }
    if (strcmp(input, "pasted-status") == 0) {
        state->saw_complete = true;
        return "PASTE-STATUS-READY";
    }
    state->saw_partial = true;
    return NULL;
}

static void queue_test_notifications(void) {
    if (!ic_queue_notification("NOTICE-ONE [b]\n") || !ic_queue_notification("NOTICE-TWO")) {
        exit(9);
    }
}

static bool notification_runoff_handler(ic_keycode_t key, void* arg) {
    (void)arg;
    if (key != IC_KEY_F6) {
        return false;
    }
    queue_test_notifications();
    return true;
}

static bool notification_submit_handler(const char* input, void* arg) {
    (void)input;
    (void)arg;
    queue_test_notifications();
    return true;
}

// Keep the first Enter in the editor, then submit after its automatic
// continuation has created a second line.
static bool auto_indent_continuation_handler(const char* input, void* arg) {
    (void)arg;
    return input != NULL && strchr(input, '\n') != NULL;
}

static const char* menu_dismiss_choices[] = {"choiceone", "choicetwo", NULL};

static void menu_dismiss_word_provider(ic_completion_env_t* cenv, const char* prefix) {
    (void)ic_add_completions(cenv, prefix, menu_dismiss_choices);
}

static void menu_dismiss_completer(ic_completion_env_t* cenv, const char* prefix) {
    ic_complete_word(cenv, prefix, menu_dismiss_word_provider, NULL);
}

static bool menu_dismiss_action(const char* choice) {
    const char* buffer = ic_get_buffer();
    if (buffer == NULL || strcmp(buffer, "keep") != 0 || !ic_suspend_readline_terminal()) {
        return false;
    }
    // Match shell palette commands that run with the editor's terminal suspended.
    // The PTY test inspects the screen before this action writes anything.
    (void)printf("[IC_MENU_ACTION_BEGIN]\n");
    (void)fflush(stdout);
    return ic_resume_readline_terminal() && ic_set_buffer(choice);
}

static bool menu_dismiss_palette_handler(const ic_command_palette_entry_t* entry, void* arg) {
    (void)arg;
    return menu_dismiss_action(entry->id);
}

static bool menu_dismiss_custom_handler(ic_keycode_t key, void* arg) {
    (void)arg;
    if (key != IC_KEY_F3) {
        return false;
    }
    const ic_menu_item_t items[] = {
        {"choiceone", "MENU-DETAIL-ONE", ""},
        {"choicetwo", "MENU-DETAIL-TWO", ""},
    };
    size_t selected = 0;
    if (!ic_show_menu("custom actions: ", items, 2, &selected)) {
        return true;
    }
    return menu_dismiss_action(menu_dismiss_choices[selected]);
}

static bool menu_dismiss_submit_handler(const char* input, void* arg) {
    (void)input;
    (void)arg;
    // Observe dismissal before readline's final cleanup can hide leftover rows.
    (void)printf("[IC_MENU_SUBMIT]");
    (void)fflush(stdout);
    return true;
}

static bool pty_custom_menu_runoff_handler(ic_keycode_t key, void* arg) {
    (void)arg;
    if (key != IC_KEY_F3) {
        return false;
    }

    static const ic_menu_item_t items[] = {
        {"Show status", "inspect the working tree", "git changes"},
        {"Restart service",
         "restart the background worker after it finishes outstanding requests; "
         "CUSTOM-MENU-EXPANDED-DESCRIPTION",
         "reload daemon"},
        {"Open logs", "view recent service output", "tail diagnostics"},
    };
    static const char* const values[] = {"status", "restart", "logs"};
    size_t selected = 0;
    if (ic_show_menu("custom actions: ", items, sizeof(items) / sizeof(items[0]), &selected)) {
        (void)ic_set_buffer(values[selected]);
    }
    return true;
}

static bool pty_menu_viewport_handler(ic_keycode_t key, void* arg) {
    if (key != IC_KEY_F3) {
        return false;
    }

    char labels[120][16];
    ic_menu_item_t items[120] = {0};
    for (size_t i = 0; i < 120; ++i) {
        (void)snprintf(labels[i], sizeof(labels[i]), "entry%03zu", i);
        items[i].label = labels[i];
    }
    if (arg != NULL) {
        items[0].description = "preview first line\npreview second line\npreview third line";
    }
    size_t selected = 0;
    if (ic_show_menu("custom actions: ", items, 120, &selected)) {
        (void)ic_set_buffer(labels[selected]);
    }
    return true;
}

static void pty_menu_viewport_completer(ic_completion_env_t* cenv, const char* prefix) {
    for (int i = 0; i < 120; ++i) {
        char entry[16];
        (void)snprintf(entry, sizeof(entry), "entry%03d", i);
        const char* words[] = {entry, NULL};
        if (!ic_add_completions(cenv, prefix, words)) {
            break;
        }
    }
}

static void pty_completion_word_provider(ic_completion_env_t* cenv, const char* prefix) {
    if (g_notify_from_completion) {
        g_notify_from_completion = false;
        queue_test_notifications();
    }
    if (g_completion_mode == COMPLETION_MODE_SINGLE) {
        static const char* single_words[] = {"hello", NULL};
        (void)ic_add_completions(cenv, prefix, single_words);
        return;
    }
    if (g_completion_mode == COMPLETION_MODE_DUAL) {
        static const char* dual_words[] = {"planet", "planar", NULL};
        (void)ic_add_completions(cenv, prefix, dual_words);
        return;
    }
    if (g_completion_mode == COMPLETION_MODE_MANY) {
        static const char* many_words[] = {
            "s01", "s02", "s03", "s04", "s05", "s06", "s07",
            "s08", "s09", "s10", "s11", "s12", NULL,
        };
        (void)ic_add_completions(cenv, prefix, many_words);
        return;
    }
    if (g_completion_mode == COMPLETION_MODE_MANY_MULTILINE ||
        g_completion_mode == COMPLETION_MODE_MANY_MULTILINE_REPLACEMENT ||
        g_completion_mode == COMPLETION_MODE_MANY_TALL_REPLACEMENT) {
        static const char* many_words[] = {
            "m01", "m02", "m03", "m04", "m05", "m06", "m07", "m08", "m09", "m10", "m11", "m12",
        };
        const char* multiline = "m02 first line\nm02 second line";
        if (g_completion_mode == COMPLETION_MODE_MANY_TALL_REPLACEMENT) {
            multiline =
                "m02 first line\npreview line 02\npreview line 03\npreview line 04\n"
                "preview line 05\npreview line 06\npreview line 07\npreview line 08\n"
                "preview line 09\npreview line 10\npreview line 11\npreview line 12\n"
                "preview line 13\npreview line 14\npreview line 15\npreview line 16\n"
                "preview line 17\npreview line 18\npreview line 19\npreview line 20";
        }
        if (g_wrap_completion_input) {
            multiline =
                "m02 first line\n"
                "a long preview line that wraps across the terminal before the next newline; "
                "a long preview line that wraps across the terminal before the next newline; "
                "a long preview line that wraps across the terminal before the next newline; "
                "a long preview line that wraps across the terminal before the next newline; "
                "a long preview line that wraps across the terminal before the next newline; "
                "a long preview line that wraps across the terminal before the next newline";
        }
        char flattened[512];
        if (g_flatten_completion_display) {
            (void)snprintf(flattened, sizeof(flattened), "%s", multiline);
            for (char* p = flattened; *p != '\0'; p++) {
                if (*p == '\n') {
                    *p = ' ';
                }
            }
        }
        const long delete_before = (prefix != NULL ? (long)strlen(prefix) : 0L);
        for (size_t i = 0; i < (sizeof(many_words) / sizeof(many_words[0])); i++) {
            const char* replacement = many_words[i];
            const char* display =
                (i == 1 ? (g_flatten_completion_display ? flattened : multiline) : replacement);
            const char* source = (i == 4 ? "abbr first line\nabbr second line" : "history");
            if (i == 1 && (g_completion_mode == COMPLETION_MODE_MANY_MULTILINE_REPLACEMENT ||
                           g_completion_mode == COMPLETION_MODE_MANY_TALL_REPLACEMENT)) {
                replacement = multiline;
            }
            (void)ic_add_completion_prim_with_source(cenv, replacement, display, NULL, source,
                                                     delete_before, 0);
        }
        return;
    }
    if (g_completion_mode == COMPLETION_MODE_SPELL_SINGLE) {
        if (prefix != NULL && strcmp(prefix, "hlelo") == 0) {
            (void)ic_add_completion_prim_with_source(cenv, "hello", NULL, NULL, "spell",
                                                     (long)strlen(prefix), 0);
        }
        return;
    }
    if (g_completion_mode == COMPLETION_MODE_SPELL_MIXED) {
        if (prefix != NULL && strcmp(prefix, "hlelo") == 0) {
            const long delete_before = (long)strlen(prefix);
            (void)ic_add_completion_prim_with_source(cenv, "hello", NULL, NULL, "spell",
                                                     delete_before, 0);
            (void)ic_add_completion_prim_with_source(cenv, "hlelo-tool", NULL, NULL, "command",
                                                     delete_before, 0);
        }
        return;
    }
}

static void pty_completion_dispatcher(ic_completion_env_t* cenv, const char* prefix) {
    if (g_completion_mode == COMPLETION_MODE_SPELL_CROSS_TOKEN) {
        if (prefix != NULL && strcmp(prefix, "hlelo add") == 0) {
            (void)ic_add_completion_prim_with_source(cenv, "hello", NULL, NULL, "spell",
                                                     (long)strlen(prefix), 0);
        }
        return;
    }
    ic_complete_word(cenv, prefix, pty_completion_word_provider, NULL);
}

static void emit_result(const char* line) {
    const char* text = (line == NULL ? "" : line);
    (void)printf("\n[IC_RESULT_BEGIN]");
    if (text[0] != '\0') {
        (void)fwrite(text, sizeof(char), strlen(text), stdout);
    }
    (void)printf("[IC_RESULT_END]\n");
    (void)fflush(stdout);
}

static void emit_readline_step_done(void) {
    (void)printf("\n[IC_READLINE_STEP_DONE]\n");
    (void)fflush(stdout);
}

static void emit_typeahead_capture_ready(void) {
    (void)printf("\n[IC_TYPEAHEAD_CAPTURE_READY]\n");
    (void)fflush(stdout);
}

static bool apply_transient_prompt_on_submit(const char* input_buffer, void* arg) {
    (void)input_buffer;
    const char* final_prompt = (arg == NULL ? "final-prefix\nfinal> " : (const char*)arg);
    (void)ic_current_loop_reset(NULL, final_prompt, "final-right");
    return true;
}

static int run_readline_status_case(const char* scenario) {
    if (scenario == NULL) {
        return 2;
    }

    (void)ic_enable_multiline(false);
    (void)ic_enable_hint(false);
    (void)ic_enable_inline_help(false);
    (void)ic_enable_completion_preview(false);
    (void)ic_enable_auto_tab(false);
    (void)ic_enable_spell_correct_on_enter(false);
    (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SIMPLE);
    (void)ic_enable_mouse_clicking(false);
    (void)ic_enable_mouse_reporting_status_line(true);
    if (!ic_set_key_binding_profile("emacs")) {
        return 5;
    }

    const bool idle_completion_menu = (strcmp(scenario, "idle_completion_menu") == 0);
    const bool idle_history_menu = (strcmp(scenario, "idle_history_menu") == 0);
    const bool idle_command_palette = (strcmp(scenario, "idle_command_palette") == 0);
    const bool idle_custom_menu = (strcmp(scenario, "idle_custom_menu") == 0);
    const bool idle_case = (strcmp(scenario, "idle") == 0 || idle_completion_menu ||
                            idle_history_menu || idle_command_palette || idle_custom_menu);
    if (strcmp(scenario, "stop_event") == 0) {
        if (!ic_push_key_event(IC_KEY_EVENT_STOP)) {
            emit_result("ERR:queue-stop-event");
            return 0;
        }
    } else if (idle_case) {
        (void)ic_set_idle_timeout(idle_completion_menu || idle_history_menu ||
                                          idle_command_palette || idle_custom_menu
                                      ? 250
                                      : 75);
        if (idle_completion_menu) {
            g_completion_mode = COMPLETION_MODE_MANY;
            ic_set_default_completer(pty_completion_dispatcher, NULL);
        } else if (idle_history_menu) {
#if defined(_WIN32)
            const char* history_path = "cjsh_isocline_pty_idle_history.tmp";
#else
            const char* history_path = "/tmp/cjsh_isocline_pty_idle_history.tmp";
#endif
            (void)remove(history_path);
            ic_set_history(history_path, 20);
            ic_history_clear();
            ic_history_add("keep history");
        } else if (idle_custom_menu) {
            if (!ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_RUNOFF)) {
                return 6;
            }
            ic_set_unhandled_key_handler(pty_custom_menu_runoff_handler, NULL);
        }
    } else if (strcmp(scenario, "text") != 0 && strcmp(scenario, "ctrl_c") != 0 &&
               strcmp(scenario, "ctrl_d") != 0) {
        return 2;
    }

    const char* idle_input = (idle_completion_menu ? "s" : "abc\n");
    size_t idle_cursor = 1;
    if (idle_history_menu || idle_command_palette || idle_custom_menu) {
        idle_input = "keep";
        idle_cursor = 4;
    }
    ic_readline_result_t result =
        (idle_case ? ic_readline_with_status_at_cursor("pty", NULL, idle_input, idle_cursor)
                   : ic_readline_with_status("pty", NULL, NULL));

    const char* line = (result.input == NULL ? "" : result.input);
    char payload[512];
    if (idle_case) {
        (void)snprintf(payload, sizeof(payload), "%s|%s|cursor=%zu|tty=%d|lost=%d",
                       ic_readline_disposition_name(result.disposition), line, result.cursor_pos,
                       (result.tty_active ? 1 : 0), (result.tty_lost ? 1 : 0));
    } else {
        (void)snprintf(payload, sizeof(payload), "%s|%s|tty=%d|lost=%d",
                       ic_readline_disposition_name(result.disposition), line,
                       (result.tty_active ? 1 : 0), (result.tty_lost ? 1 : 0));
    }
    emit_result(payload);

    if (result.input != NULL) {
        ic_free(result.input);
    }

    return 0;
}

static int run_history_probe_case(const char* scenario) {
#if defined(_WIN32)
    const char* history_path = "cjsh_isocline_pty_history.tmp";
#else
    char history_path[128];
    (void)snprintf(history_path, sizeof(history_path), "/tmp/cjsh_isocline_pty_history_%ld.tmp",
                   (long)getpid());
#endif
    (void)remove(history_path);
    ic_set_history(history_path, 200);
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->history == NULL) {
        emit_result("ERR:no-env");
        return 0;
    }

    ic_history_clear();
    ic_history_add("echo one");
    ic_history_add("echo two");

    if (strcmp(scenario, "history_probe_latest") == 0) {
        const char* latest = history_get(env->history, 0);
        emit_result(latest);
        return 0;
    }
    if (strcmp(scenario, "history_probe_previous") == 0) {
        const char* prev = history_get(env->history, 1);
        emit_result(prev);
        return 0;
    }
    if (strcmp(scenario, "history_probe_remove_last") == 0) {
        ic_history_remove_last();
        const char* latest = history_get(env->history, 0);
        emit_result(latest);
        return 0;
    }
    if (strcmp(scenario, "history_probe_count") == 0) {
        char buf[32];
        (void)snprintf(buf, sizeof(buf), "%zd", history_count(env->history));
        emit_result(buf);
        return 0;
    }

    emit_result("ERR:unknown-probe");
    return 0;
}

static bool queue_raw_bytes(const uint8_t* bytes, size_t count) {
    if (bytes == NULL && count > 0) {
        return false;
    }
    return ic_push_raw_input(bytes, count);
}

#if !defined(_WIN32)
typedef struct delayed_raw_feed_s {
    const uint8_t* bytes;
    size_t count;
    unsigned int delay_us;
} delayed_raw_feed_t;

static void* delayed_raw_feed_thread(void* arg) {
    delayed_raw_feed_t* feed = (delayed_raw_feed_t*)arg;
    if (feed == NULL) {
        return NULL;
    }

    struct timespec req = {
        .tv_sec = feed->delay_us / 1000000u,
        .tv_nsec = (long)((feed->delay_us % 1000000u) * 1000u),
    };
    (void)nanosleep(&req, NULL);

    (void)ic_push_raw_input(feed->bytes, feed->count);
    return NULL;
}
#endif

#if !defined(_WIN32)
static int run_paste_wakeup_case(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL || env->tty == NULL || !tty_start_raw(env->tty)) {
        return 9;
    }

    code_t code;
    const uint8_t start[] = "\x1b[200~";
    (void)tty_replay_typeahead(env->tty, start, sizeof(start) - 1);
    if (!tty_read_timeout(env->tty, -1, &code) || code != IC_KEY_PASTE_START) {
        return 9;
    }

    // Consuming a pushed key leaves its wake byte queued. The end marker starts
    // in replay and finishes on the PTY, exercising the escape decoder's reads.
    tty_code_pushback(env->tty, 'x');
    if (!tty_read_timeout(env->tty, -1, &code) || code != 'x') {
        return 9;
    }
    const uint8_t escape = '\x1b';
    (void)tty_replay_typeahead(env->tty, &escape, 1);
    emit_typeahead_capture_ready();

    int ready;
    do {
        fd_set input;
        FD_ZERO(&input);
        FD_SET(STDIN_FILENO, &input);
        struct timeval timeout = {.tv_sec = 4, .tv_usec = 0};
        ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    } while (ready < 0 && errno == EINTR);

    const bool ended =
        ready > 0 && tty_read_timeout(env->tty, -1, &code) && code == IC_KEY_PASTE_END;
    tty_end_raw(env->tty);
    emit_result(ended ? "paste-ended" : "broken-end-marker");
    return 0;
}
#endif

static int run_case(const char* scenario) {
    if (scenario == NULL) {
        return 2;
    }

    if (strcmp(scenario, "paste_status_callback") == 0) {
        paste_status_t state = {false, false};
        ic_set_status_message_callback(paste_status_callback, &state);
        char* line = ic_readline("pty> ", NULL, NULL);
        ic_set_status_message_callback(NULL, NULL);
        const bool complete = line != NULL && strcmp(line, "pasted-status") == 0 &&
                              state.saw_complete && !state.saw_partial;
        emit_result(complete ? "batched-status" : "partial-status");
        ic_free(line);
        return 0;
    }

#if !defined(_WIN32)
    if (strcmp(scenario, "typeahead_capture_empty") == 0) {
        ic_env_t* env = ic_get_env();
        if (env == NULL || env->tty == NULL) {
            return 9;
        }
        (void)ic_enable_typeahead(true);
        const bool captured = ic_typeahead_capture_available_input();
        emit_result(!captured && !tty_lost_terminal(env->tty) ? "terminal-active"
                                                              : "terminal-lost");
        return 0;
    }
    if (strcmp(scenario, "typeahead_capture_paste_wakeup") == 0) {
        return run_paste_wakeup_case();
    }
#endif

    if (strncmp(scenario, "status_", 7) == 0) {
        return run_readline_status_case(scenario + 7);
    }

    const bool use_vim_profile = (strncmp(scenario, "vim_", 4) == 0);
    if (use_vim_profile) {
        scenario += 4;
    }

    if (strncmp(scenario, "history_probe_", 14) == 0) {
        return run_history_probe_case(scenario);
    }

    const bool region_marking =
        (strcmp(scenario, "region_marking") == 0 ||
         strcmp(scenario, "region_marking_multiline") == 0 ||
         strcmp(scenario, "region_marking_transient_prompt_components") == 0 ||
         strcmp(scenario, "prompt_guard_region_marking_external_visible") == 0);
    bool multiline_mode =
        (strncmp(scenario, "typeahead_", 10) == 0 ||
         strcmp(scenario, "multiline_ctrl_j_insert_newline") == 0 ||
         strcmp(scenario, "multiline_backslash_continuation") == 0 ||
         strcmp(scenario, "multiline_backslash_continuation_retained") == 0 ||
         strcmp(scenario, "multiline_auto_continuation_indent") == 0 ||
         strcmp(scenario, "multiline_auto_continuation_indent_disabled") == 0 ||
         strcmp(scenario, "multiline_backslash_submit_with_following_content") == 0 ||
         strcmp(scenario, "multiline_initial_ctrl_j") == 0 ||
         strcmp(scenario, "multiline_ctrl_a_stays_on_line") == 0 ||
         strcmp(scenario, "multiline_ctrl_e_stays_on_line") == 0 ||
         strcmp(scenario, "multiline_max_lines_viewport") == 0 ||
         strcmp(scenario, "multiline_max_lines_prompt_reset") == 0 ||
         strcmp(scenario, "multiline_terminal_row_cap") == 0 ||
         strcmp(scenario, "completion_many_menu_long_multiline") == 0 ||
         strcmp(scenario, "history_search_long_multiline_viewport") == 0 ||
         strcmp(scenario, "region_marking_multiline") == 0 ||
         strcmp(scenario, "completion_many_menu_multiline_replacement") == 0);
    const bool tall_completion_case =
        (strcmp(scenario, "completion_many_menu_tall_replacement") == 0 ||
         strcmp(scenario, "completion_many_menu_tall_flattened") == 0 ||
         strcmp(scenario, "completion_many_menu_tall_wrapped_input") == 0 ||
         strcmp(scenario, "completion_many_menu_tall_marker_off") == 0 ||
         strcmp(scenario, "completion_many_menu_tall_prompt_prefix") == 0);
    if (tall_completion_case) {
        multiline_mode = true;
    }
    (void)ic_enable_multiline(multiline_mode);
    (void)ic_enable_multiline_continuation_retention(
        strcmp(scenario, "multiline_backslash_continuation_retained") == 0);
    (void)ic_enable_hint(false);
    (void)ic_enable_inline_help(false);
    (void)ic_enable_completion_preview(false);
    (void)ic_enable_auto_tab(false);
    (void)ic_enable_completion_click_accept(true);
    (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SIMPLE);
    (void)ic_enable_mouse_clicking(false);
    (void)ic_enable_mouse_reporting_status_line(true);
    if (!ic_set_key_binding_profile(use_vim_profile ? "vim" : "emacs")) {
        return 5;
    }
#if defined(_WIN32)
    const char* history_path = "cjsh_isocline_pty_history.tmp";
#else
    char history_path[128];
    (void)snprintf(history_path, sizeof(history_path), "/tmp/cjsh_isocline_pty_history_%ld.tmp",
                   (long)getpid());
#endif
    (void)remove(history_path);
    ic_set_history(history_path, 200);
    g_completion_mode = COMPLETION_MODE_NONE;
    ic_set_default_completer(NULL, NULL);

    const char* initial_input = NULL;
    const char* inline_right_text = NULL;
    const char* pre_prompt_output = NULL;
    const char* prompt_text = "pty";
    const char* prompt_marker = NULL;
    const char* continuation_prompt_marker = NULL;
    bool history_interactive_triplet = false;
    bool history_sort_cycle_nonpersistent = false;
    bool external_pre_prompt_output = false;
    bool typeahead_two_readlines = false;
    const bool capture_typeahead_from_pty = (strncmp(scenario, "typeahead_capture_", 18) == 0);
    if (strncmp(scenario, "menu_dismiss_", 13) == 0) {
        if (strstr(scenario, "_multiline_prompt") != NULL) {
            prompt_text = "MENU-BASE-TOP\nMENU-BASE-MIDDLE\npty";
            inline_right_text = "MENU-BASE-RIGHT";
        }
        (void)ic_enable_inline_help(true);
        (void)ic_set_status_hint_mode(IC_STATUS_HINT_OFF);
        (void)ic_enable_mouse_reporting_status_line(false);
        (void)ic_enable_mouse_clicking(true);
        ic_set_check_for_continuation_or_return_callback(menu_dismiss_submit_handler, NULL);
        if (strstr(scenario, "_completion") != NULL) {
            ic_set_default_completer(menu_dismiss_completer, NULL);
        } else if (strstr(scenario, "_history") != NULL) {
            ic_history_clear();
            ic_history_add("choicetwo");
            ic_history_add("choiceone");
        } else if (strstr(scenario, "_palette") != NULL) {
            initial_input = "keep";
            const ic_command_palette_entry_t entries[] = {
                {"choiceone", "choiceone", "MENU-DETAIL-ONE", "zzdismiss"},
                {"choicetwo", "choicetwo", "MENU-DETAIL-TWO", "zzdismiss"},
            };
            if (!ic_set_command_palette_entries(entries, 2)) {
                return 6;
            }
            ic_set_command_palette_entry_handler(menu_dismiss_palette_handler, NULL);
        } else if (strstr(scenario, "_custom") != NULL) {
            initial_input = "keep";
            if (!ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_RUNOFF)) {
                return 6;
            }
            ic_set_unhandled_key_handler(menu_dismiss_custom_handler, NULL);
        }
    } else if (strncmp(scenario, "notification_", 13) == 0) {
        initial_input = "ab";
        prompt_text = "NOTICE-TOP\npty";
        inline_right_text = "NOTICE-RIGHT";
        (void)ic_set_status_hint_mode(IC_STATUS_HINT_PERSISTENT);
        ic_set_unhandled_key_handler(notification_runoff_handler, NULL);
        (void)ic_bind_key(IC_KEY_F6, IC_KEY_ACTION_RUNOFF);
        if (strcmp(scenario, "notification_completion") == 0) {
            initial_input = "pla";
            g_notify_from_completion = true;
            g_completion_mode = COMPLETION_MODE_DUAL;
            ic_set_default_completer(pty_completion_dispatcher, NULL);
        } else if (strcmp(scenario, "notification_submit") == 0) {
            ic_set_check_for_continuation_or_return_callback(notification_submit_handler, NULL);
        } else if (strcmp(scenario, "notification_multiline") == 0) {
            (void)ic_enable_multiline(true);
            (void)ic_enable_line_numbers(false);
            initial_input = "first\nsecond\nthird\nfourth\nfifth\nsixth\nseventh";
            (void)ic_set_multiline_max_line_count(3);
        }
    } else if (strncmp(scenario, "line_wrap_marker_", 17) == 0) {
        initial_input = "abcdefghijklmnopqrstuvwxyz0123456789";
        (void)ic_enable_multiline(true);
        (void)ic_enable_line_numbers(false);
        if (strstr(scenario, "_empty") != NULL || strstr(scenario, "_restored") != NULL) {
            (void)ic_set_line_wrap_marker("");
        }
        if (strstr(scenario, "_restored") != NULL) {
            (void)ic_set_line_wrap_marker(NULL);
        } else if (strstr(scenario, "_ascii") != NULL) {
            (void)ic_set_line_wrap_marker("!");
        } else if (strstr(scenario, "_unicode") != NULL) {
            (void)ic_set_line_wrap_marker("↪");
        } else if (strstr(scenario, "_wide") != NULL) {
            (void)ic_set_line_wrap_marker("界");
        } else if (strstr(scenario, "_bracket") != NULL) {
            (void)ic_set_line_wrap_marker("[");
        }
        if (strstr(scenario, "_boundary") != NULL) {
            initial_input = "";
            inline_right_text = "right";
        }
    } else if (strcmp(scenario, "cursor_move_insert") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "home_insert") == 0) {
        initial_input = "bc";
    } else if (strcmp(scenario, "end_insert") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "backspace_at_start_noop") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "delete_at_end_noop") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "kill_to_end_at_end_noop") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "kill_to_start_at_start_noop") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "left_boundary_insert") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "right_boundary_insert") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "append_to_initial_input") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "ctrl_l_redraw_keeps_buffer") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "ctrl_a_ctrl_e_append") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "ctrl_d_at_end_noop") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "undo_single_change") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "undo_redo_roundtrip") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "undo_after_kill_to_end") == 0) {
        initial_input = "abcdef";
    } else if (strcmp(scenario, "redo_cleared_by_new_edit") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "resize_reflow_initial_input") == 0) {
        initial_input = "abcdefghij";
    } else if (strcmp(scenario, "shell_prompt_wrap_boundary") == 0) {
        prompt_text = "pty> CJsShell git:(master) x ";
        prompt_marker = "";
        continuation_prompt_marker = "> ";
    } else if (strcmp(scenario, "multiline_initial_ctrl_j") == 0) {
        initial_input = "ab";
    } else if (strcmp(scenario, "multiline_backslash_submit_with_following_content") == 0) {
        initial_input = "echo \\\nhi";
    } else if (strcmp(scenario, "multiline_auto_continuation_indent") == 0) {
        ic_set_check_for_continuation_or_return_callback(auto_indent_continuation_handler, NULL);
    } else if (strcmp(scenario, "multiline_auto_continuation_indent_disabled") == 0) {
        ic_set_check_for_continuation_or_return_callback(auto_indent_continuation_handler, NULL);
        (void)ic_enable_multiline_indent(false);
    } else if (strcmp(scenario, "multiline_ctrl_a_stays_on_line") == 0) {
        initial_input = "ab\ncd\nef";
    } else if (strcmp(scenario, "multiline_ctrl_e_stays_on_line") == 0) {
        initial_input = "ab\ncd\nef";
    } else if (strcmp(scenario, "multiline_max_lines_viewport") == 0 ||
               strcmp(scenario, "multiline_max_lines_prompt_reset") == 0) {
        initial_input =
            "viewport-line-01\nviewport-line-02\nviewport-line-03\nviewport-line-04\n"
            "viewport-line-05";
        (void)ic_set_multiline_max_line_count(3);
        (void)ic_enable_line_numbers_with_continuation_prompt(true);
    } else if (strcmp(scenario, "multiline_terminal_row_cap") == 0) {
        prompt_text = "PREFIX-TOP\nPREFIX-MIDDLE\npty";
        initial_input =
            "terminal-line-01\nterminal-line-02\nterminal-line-03\nterminal-line-04\n"
            "terminal-line-05";
        (void)ic_set_multiline_max_line_count(15);
    } else if (strcmp(scenario, "completion_midline_single") == 0) {
        initial_input = "say he";
        g_completion_mode = COMPLETION_MODE_SINGLE;
        ic_set_default_completer(pty_completion_dispatcher, NULL);
    } else if (strcmp(scenario, "enter_spell_single_disabled") == 0 ||
               strcmp(scenario, "enter_spell_single_enabled") == 0 ||
               strcmp(scenario, "spell_status_delayed") == 0) {
        g_completion_mode = COMPLETION_MODE_SPELL_SINGLE;
        ic_set_default_completer(pty_completion_dispatcher, NULL);
        if (strcmp(scenario, "enter_spell_single_enabled") == 0) {
            (void)ic_enable_spell_correct_on_enter(true);
        }
        if (strcmp(scenario, "spell_status_delayed") == 0) {
            (void)ic_enable_hint(true);
            (void)ic_set_hint_delay(250);
        }
    } else if (strcmp(scenario, "spell_status_cross_token") == 0) {
        g_completion_mode = COMPLETION_MODE_SPELL_CROSS_TOKEN;
        ic_set_default_completer(pty_completion_dispatcher, NULL);
        (void)ic_enable_hint(true);
    } else if (strcmp(scenario, "completion_spell_mixed_tab") == 0) {
        g_completion_mode = COMPLETION_MODE_SPELL_MIXED;
        ic_set_default_completer(pty_completion_dispatcher, NULL);
    } else if (strcmp(scenario, "hint_clears_on_empty_line") == 0) {
        g_completion_mode = COMPLETION_MODE_SINGLE;
        (void)ic_enable_hint(true);
        (void)ic_set_hint_delay(0);
        ic_set_default_completer(pty_completion_dispatcher, NULL);
    } else if (strncmp(scenario, "menu_viewport_", 14) == 0) {
        if (strstr(scenario, "_mouse") != NULL) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY);
        } else if (strstr(scenario, "_smart") != NULL) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SMART);
            (void)ic_enable_mouse_clicking(true);
        }
        if (strstr(scenario, "_passive") != NULL) {
            (void)ic_enable_completion_auto_menu(true);
        }
        size_t limit = 50;  // explicit viewport fixture, independent of production defaults
        if (strstr(scenario, "_default") != NULL) {
            if (strstr(scenario, "_completion") != NULL) {
                limit = ic_get_completion_menu_max_line_count();
            } else if (strstr(scenario, "_history") != NULL) {
                limit = ic_get_history_menu_max_line_count();
            } else if (strstr(scenario, "_palette") != NULL) {
                limit = ic_get_command_palette_max_line_count();
            } else {
                limit = ic_get_custom_menu_max_line_count();
            }
        } else if (strstr(scenario, "_limit") != NULL) {
            limit = 8;
        } else if (strstr(scenario, "_large") != NULL) {
            limit = 75;
        } else if (strstr(scenario, "_single") != NULL) {
            limit = 1;
        }
        // Deliberately give the other menus different limits to catch cross-menu coupling.
        (void)ic_set_completion_menu_max_line_count(2);
        (void)ic_set_history_menu_max_line_count(3);
        (void)ic_set_command_palette_max_line_count(4);
        (void)ic_set_custom_menu_max_line_count(5);
        if (strstr(scenario, "_completion") != NULL) {
            (void)ic_set_completion_menu_max_line_count(limit);
        } else if (strstr(scenario, "_history") != NULL) {
            (void)ic_set_history_menu_max_line_count(limit);
        } else if (strstr(scenario, "_palette") != NULL) {
            (void)ic_set_command_palette_max_line_count(limit);
        } else {
            (void)ic_set_custom_menu_max_line_count(limit);
        }
        if (strstr(scenario, "_completion") != NULL) {
            ic_set_default_completer(pty_menu_viewport_completer, NULL);
        } else if (strstr(scenario, "_history") != NULL) {
            ic_history_clear();
            for (int i = 119; i >= 0; --i) {
                char entry[16];
                (void)snprintf(entry, sizeof(entry), "entry%03d", i);
                ic_history_add(entry);
            }
        } else if (strstr(scenario, "_palette") != NULL) {
            char labels[120][16];
            ic_command_palette_entry_t entries[120] = {0};
            for (size_t i = 0; i < 120; ++i) {
                (void)snprintf(labels[i], sizeof(labels[i]), "entry%03zu", i);
                entries[i].id = labels[i];
                entries[i].name = labels[i];
                entries[i].keywords = "zzviewport";
            }
            if (!ic_set_command_palette_entries(entries, 120)) {
                return 6;
            }
        } else if (strncmp(scenario, "menu_viewport_custom", 20) == 0) {
            if (!ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_RUNOFF)) {
                return 6;
            }
            const bool preview = (strstr(scenario, "_preview") != NULL);
            ic_set_unhandled_key_handler(pty_menu_viewport_handler,
                                         preview ? &g_completion_mode : NULL);
            if (strcmp(scenario, "menu_viewport_custom_no_margin") == 0) {
                (void)ic_set_multiline_bottom_line_count(0);
            }
        }
    } else if (strncmp(scenario, "completion_auto_menu", 20) == 0) {
        (void)ic_enable_completion_auto_menu(strstr(scenario, "_off") == NULL);
        (void)ic_enable_completion_preview(strstr(scenario, "_nopreview") == NULL);
        (void)ic_enable_completion_click_accept(strstr(scenario, "_selectonly") == NULL);
        (void)ic_enable_hint(strstr(scenario, "_hints") != NULL);
        (void)ic_enable_auto_tab(strstr(scenario, "_autotab") != NULL);
        (void)ic_set_status_hint_mode(IC_STATUS_HINT_OFF);
        (void)ic_enable_mouse_reporting_status_line(false);
        (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY);
        if (strstr(scenario, "_mouse") != NULL) {
            (void)ic_set_mouse_clicking_mode(strstr(scenario, "_smart") != NULL
                                                 ? IC_MOUSE_CLICKING_SMART
                                                 : IC_MOUSE_CLICKING_SIMPLE);
            (void)ic_enable_mouse_clicking(true);
        }
        if (strstr(scenario, "_multiline") != NULL) {
            (void)ic_enable_multiline(true);
            initial_input = "echo\nx";
        }
        if (strstr(scenario, "_prefix") != NULL) {
            prompt_text = "AUTO-MENU-PREFIX\npty";
        }
        if (strstr(scenario, "_limit") != NULL) {
            (void)ic_set_completion_menu_max_line_count(3);
        }
        if (strstr(scenario, "_single") != NULL) {
            g_completion_mode = COMPLETION_MODE_SINGLE;
        } else if (strstr(scenario, "_dual") != NULL) {
            g_completion_mode = COMPLETION_MODE_DUAL;
        } else if (strstr(scenario, "_spell") != NULL) {
            g_completion_mode = COMPLETION_MODE_SPELL_SINGLE;
        } else {
            g_completion_mode = COMPLETION_MODE_MANY;
        }
        ic_set_default_completer(pty_completion_dispatcher, NULL);
    } else if (strcmp(scenario, "completion_many_menu") == 0 ||
               strcmp(scenario, "completion_many_menu_preview") == 0 ||
               strcmp(scenario, "completion_many_menu_off") == 0 ||
               strcmp(scenario, "completion_many_menu_all_off") == 0 ||
               strcmp(scenario, "completion_many_menu_custom_mouse_toggle") == 0 ||
               strcmp(scenario, "completion_many_menu_mouse_default_on") == 0 ||
               strcmp(scenario, "completion_many_menu_smart") == 0 ||
               strcmp(scenario, "completion_many_menu_multiline") == 0 ||
               strcmp(scenario, "completion_many_menu_long_multiline") == 0 ||
               strcmp(scenario, "completion_many_menu_multiline_replacement") == 0 ||
               tall_completion_case) {
        if (strcmp(scenario, "completion_many_menu_multiline") == 0) {
            g_completion_mode = COMPLETION_MODE_MANY_MULTILINE;
        } else if (strcmp(scenario, "completion_many_menu_multiline_replacement") == 0) {
            g_completion_mode = COMPLETION_MODE_MANY_MULTILINE_REPLACEMENT;
            (void)ic_enable_completion_preview(true);
        } else if (tall_completion_case) {
            g_completion_mode = COMPLETION_MODE_MANY_TALL_REPLACEMENT;
            (void)ic_enable_completion_preview(true);
            g_flatten_completion_display =
                (strcmp(scenario, "completion_many_menu_tall_flattened") == 0);
            g_wrap_completion_input =
                (strcmp(scenario, "completion_many_menu_tall_wrapped_input") == 0 ||
                 strcmp(scenario, "completion_many_menu_tall_marker_off") == 0);
            if (strcmp(scenario, "completion_many_menu_tall_marker_off") == 0) {
                (void)ic_set_line_wrap_marker("");
            }
            if (strcmp(scenario, "completion_many_menu_tall_prompt_prefix") == 0) {
                prompt_text = "COMPLETION-PREFIX-TOP\nCOMPLETION-PREFIX-MIDDLE\npty";
                (void)ic_enable_line_numbers_with_continuation_prompt(true);
            }
        } else {
            g_completion_mode = COMPLETION_MODE_MANY;
        }
        if (strcmp(scenario, "completion_many_menu_preview") == 0) {
            (void)ic_enable_completion_preview(true);
        }
        if (strcmp(scenario, "completion_many_menu_long_multiline") == 0) {
            initial_input =
                "hidden-menu-line-01\nhidden-menu-line-02\nvisible-menu-line-03\n"
                "visible-menu-line-04\ns";
            (void)ic_set_multiline_max_line_count(3);
            (void)ic_enable_line_numbers_with_continuation_prompt(true);
        }
        ic_set_default_completer(pty_completion_dispatcher, NULL);
        if (strcmp(scenario, "completion_many_menu_off") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY);
        } else if (strcmp(scenario, "completion_many_menu_all_off") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_DISABLED);
        } else if (strcmp(scenario, "completion_many_menu_custom_mouse_toggle") == 0) {
            if (!ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING)) {
                return 6;
            }
            if (!ic_bind_key(IC_KEY_F2, IC_KEY_ACTION_NONE)) {
                return 6;
            }
        } else if (strcmp(scenario, "completion_many_menu_mouse_default_on") == 0) {
            (void)ic_enable_mouse_clicking(true);
        } else if (strcmp(scenario, "completion_many_menu_smart") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SMART);
            (void)ic_enable_mouse_clicking(true);
        }
    } else if (strcmp(scenario, "history_search_scroll") == 0 ||
               strcmp(scenario, "history_search_menu_off") == 0 ||
               strcmp(scenario, "history_search_all_off") == 0 ||
               strcmp(scenario, "history_search_footer") == 0) {
        initial_input = "history";
        if (strcmp(scenario, "history_search_menu_off") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY);
        } else if (strcmp(scenario, "history_search_all_off") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_DISABLED);
        } else {
            (void)ic_enable_mouse_clicking(true);
        }
        ic_history_clear();
        if (strcmp(scenario, "history_search_footer") == 0) {
            (void)ic_enable_inline_help(true);
            for (int i = 1; i <= 30; ++i) {
                char entry[32];
                (void)snprintf(entry, sizeof(entry), "history entry %02d", i);
                ic_history_add(entry);
            }
        } else {
            ic_history_add("history alpha");
            ic_history_add("history beta");
        }
    } else if (strcmp(scenario, "history_search_typed_buffer") == 0) {
        ic_history_clear();
        ic_history_add("history alpha");
        ic_history_add("history beta");
        ic_history_add("history");
    } else if (strcmp(scenario, "history_search_multiline") == 0) {
        initial_input = "mlhist";
        ic_history_clear();
        ic_history_add("printf done");
        ic_history_add("mlhist first line\nmlhist second line");
    } else if (strcmp(scenario, "history_search_tall_multiline") == 0) {
        initial_input = "tallhist";
        (void)ic_enable_inline_help(true);
        ic_history_clear();
        ic_history_add(
            "tallhist line 01\ntallhist line 02\ntallhist line 03\ntallhist line 04\n"
            "tallhist line 05\ntallhist line 06\ntallhist line 07\ntallhist line 08\n"
            "tallhist line 09\ntallhist line 10\ntallhist line 11\ntallhist line 12");
    } else if (strcmp(scenario, "history_search_long_multiline_viewport") == 0) {
        initial_input =
            "history-line-01\nhistory-line-02\nhistory-line-03\nhistory-line-04\nhistory-line-05";
        (void)ic_set_multiline_max_line_count(3);
        (void)ic_enable_line_numbers_with_continuation_prompt(true);
        ic_history_clear();
        ic_history_add("history candidate one");
        ic_history_add("history candidate two");
    } else if (strcmp(scenario, "history_search_multiline_prompt") == 0) {
        prompt_text = "MENU-BASE-TOP\nMENU-BASE-MIDDLE\npty";
        inline_right_text = "MENU-BASE-RIGHT";
        initial_input = "history";
        ic_history_clear();
        ic_history_add("history alpha");
        ic_history_add("history beta");
    } else if (strcmp(scenario, "command_palette_multiline_prompt") == 0) {
        prompt_text = "MENU-BASE-TOP\nMENU-BASE-MIDDLE\npty";
        inline_right_text = "MENU-BASE-RIGHT";
        initial_input = "keep";
    } else if (strcmp(scenario, "custom_menu_runoff") == 0 ||
               strcmp(scenario, "custom_menu_mouse_focus") == 0) {
        initial_input = "keep";
        if (!ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_RUNOFF)) {
            return 6;
        }
        ic_set_unhandled_key_handler(pty_custom_menu_runoff_handler, NULL);
        if (strcmp(scenario, "custom_menu_mouse_focus") == 0) {
            (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY);
        }
    } else if (strcmp(scenario, "transient_prompt_multiline_clear") == 0) {
        prompt_text = "ORIGINAL-TOP\nORIGINAL-MIDDLE\npty";
    } else if (strcmp(scenario, "history_search_sort_alt_s") == 0) {
        initial_input = "a";
        ic_history_clear();
        ic_history_add("banana");
        ic_history_add("apple");
        ic_history_add("carrot");
    } else if (strcmp(scenario, "history_search_timestamp_without_exit_code") == 0) {
        initial_input = "timestamp-only";
        ic_history_clear();
        const ic_history_metadata_t timestamp_only[] = {{"timestamp", "timestamp-only-value"}};
        ic_history_add_with_metadata("timestamp-only entry", timestamp_only,
                                     sizeof(timestamp_only) / sizeof(timestamp_only[0]));
    } else if (strcmp(scenario, "history_search_expanded_metadata") == 0) {
        initial_input = "metadata-rich";
        ic_history_clear();
        const ic_history_metadata_t metadata[] = {
            {"timestamp", "timestamp-label-value"},
            {"frequency", "7"},
            {"code", "2"},
            {"working_directory", "/tmp/metadata-example"},
            {"note", "first note line\nsecond note line"},
            {"long_detail",
             "a deliberately long metadata value that keeps going so the selected history "
             "preview must wrap it instead of discarding the remainder of the value when it "
             "passes the compact tag width and fixed-buffer boundaries; tail-visible"},
        };
        ic_history_add_with_metadata("metadata-rich entry", metadata,
                                     sizeof(metadata) / sizeof(metadata[0]));
    } else if (strcmp(scenario, "history_search_sort_default_metadata") == 0) {
        ic_history_clear();
        const ic_history_metadata_t rank_two[] = {{"rank", "2"}};
        const ic_history_metadata_t rank_one[] = {{"rank", "1"}};
        const ic_history_metadata_t rank_three[] = {{"rank", "3"}};
        ic_history_add_with_metadata("rank two", rank_two, sizeof(rank_two) / sizeof(rank_two[0]));
        ic_history_add_with_metadata("rank one", rank_one, sizeof(rank_one) / sizeof(rank_one[0]));
        ic_history_add_with_metadata("rank three", rank_three,
                                     sizeof(rank_three) / sizeof(rank_three[0]));
        if (!ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_METADATA_ASC, "rank")) {
            return 6;
        }
    } else if (strcmp(scenario, "history_search_sort_cycle_metadata_tag") == 0) {
        initial_input = "rank";
        ic_history_clear();
        const ic_history_metadata_t rank_two[] = {{"rank", "2"}};
        const ic_history_metadata_t rank_one[] = {{"rank", "1"}};
        const ic_history_metadata_t rank_three[] = {{"rank", "3"}};
        ic_history_add_with_metadata("rank two", rank_two, sizeof(rank_two) / sizeof(rank_two[0]));
        ic_history_add_with_metadata("rank one", rank_one, sizeof(rank_one) / sizeof(rank_one[0]));
        ic_history_add_with_metadata("rank three", rank_three,
                                     sizeof(rank_three) / sizeof(rank_three[0]));
    } else if (strcmp(scenario, "history_search_sort_cycle_nonpersistent") == 0) {
        ic_history_clear();
        ic_history_add("banana");
        ic_history_add("apple");
        ic_history_add("carrot");
        if (!ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_COMMAND_ASC, NULL)) {
            return 6;
        }
        history_sort_cycle_nonpersistent = true;
    } else if (strcmp(scenario, "insert_backspace_mouse_default_on_hidden_status") == 0) {
        (void)ic_enable_mouse_clicking(true);
        (void)ic_enable_mouse_reporting_status_line(false);
    } else if (strcmp(scenario, "mouse_status_nonempty_buffer") == 0) {
        initial_input = "x";
        (void)ic_enable_mouse_clicking(true);
    } else if (strcmp(scenario, "smart_mouse_prompt_selection") == 0 ||
               strcmp(scenario, "smart_mouse_input_click") == 0) {
        initial_input = "abc";
        (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SMART);
        (void)ic_enable_mouse_clicking(true);
    } else if (strcmp(scenario, "simple_mouse_input_click") == 0) {
        initial_input = "abc";
        (void)ic_enable_mouse_clicking(true);
    } else if (strcmp(scenario, "smart_mouse_status_selection") == 0) {
        initial_input = "x";
        (void)ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SMART);
        (void)ic_enable_mouse_clicking(true);
    } else if (strcmp(scenario, "ctrl_k_delete_to_end") == 0) {
        initial_input = "abcdef";
    } else if (strcmp(scenario, "ctrl_k_then_type") == 0) {
        initial_input = "abcdef";
    } else if (strcmp(scenario, "ctrl_u_delete_to_start") == 0) {
        initial_input = "abcdef";
    } else if (strcmp(scenario, "ctrl_u_then_type") == 0) {
        initial_input = "abcdef";
    } else if (strcmp(scenario, "delete_mid_twice") == 0) {
        initial_input = "abcd";
    } else if (strcmp(scenario, "ctrl_d_delete_mid") == 0) {
        initial_input = "abc";
    } else if (strcmp(scenario, "ctrl_w_single_word") == 0) {
        initial_input = "alpha";
    } else if (strcmp(scenario, "history_navigation_multiline") == 0) {
        (void)ic_enable_multiline(true);
        ic_history_clear();
        ic_history_add("old first\n\nold middle\nold last");
        ic_history_add("new first\nnew middle\nnew last");
    } else if (strcmp(scenario, "history_prev") == 0 ||
               strcmp(scenario, "history_prev_prev") == 0 ||
               strcmp(scenario, "history_next_empty") == 0 ||
               strcmp(scenario, "history_prev_edit") == 0) {
        ic_history_clear();
        history_interactive_triplet = true;
    } else if (strcmp(scenario, "yank_last_arg_meta_dot") == 0 ||
               strcmp(scenario, "yank_last_arg_meta_underscore") == 0) {
        initial_input = "mv ";
        ic_history_clear();
        ic_history_add("cp source.txt \"dest dir\"");
    } else if (strcmp(scenario, "yank_last_arg_repeat") == 0) {
        initial_input = "open ";
        ic_history_clear();
        ic_history_add("cp alpha.txt beta.txt");
        ic_history_add("mv gamma.txt \"delta dir\"");
    } else if (strcmp(scenario, "insert_backspace") == 0 || strcmp(scenario, "ctrl_c") == 0 ||
               strcmp(scenario, "ctrl_d_empty") == 0 || strcmp(scenario, "region_marking") == 0 ||
               strcmp(scenario, "region_marking_multiline") == 0 ||
               strcmp(scenario, "region_marking_transient_prompt_components") == 0 ||
               strcmp(scenario, "ctrl_w_delete_word") == 0 ||
               strcmp(scenario, "backspace_twice_typed") == 0 ||
               strcmp(scenario, "ctrl_w_then_type") == 0 ||
               strcmp(scenario, "resize_reflow_typed_input") == 0 ||
               strcmp(scenario, "multiline_ctrl_j_insert_newline") == 0 ||
               strcmp(scenario, "multiline_backslash_continuation") == 0 ||
               strcmp(scenario, "multiline_backslash_continuation_retained") == 0 ||
               strcmp(scenario, "completion_single_tab") == 0 ||
               strcmp(scenario, "completion_single_then_type") == 0 ||
               strcmp(scenario, "completion_no_match") == 0 ||
               strcmp(scenario, "completion_dual_common_prefix") == 0 ||
               strcmp(scenario, "completion_dual_footer") == 0) {
        if (strcmp(scenario, "completion_single_tab") == 0 ||
            strcmp(scenario, "completion_single_then_type") == 0 ||
            strcmp(scenario, "completion_no_match") == 0) {
            g_completion_mode = COMPLETION_MODE_SINGLE;
            ic_set_default_completer(pty_completion_dispatcher, NULL);
        } else if (strcmp(scenario, "completion_dual_common_prefix") == 0 ||
                   strcmp(scenario, "completion_dual_footer") == 0) {
            g_completion_mode = COMPLETION_MODE_DUAL;
            ic_set_default_completer(pty_completion_dispatcher, NULL);
        }
        // No additional setup needed.
    } else if (strcmp(scenario, "prompt_guard_visible_text") == 0) {
        pre_prompt_output = "visible-before-prompt";
    } else if (strcmp(scenario, "prompt_guard_tab_only") == 0) {
        pre_prompt_output = "\t";
    } else if (strcmp(scenario, "prompt_guard_escape_only") == 0) {
        pre_prompt_output = "\x1B[31m\x1B[0m";
    } else if (strcmp(scenario, "prompt_guard_osc_only") == 0) {
        pre_prompt_output = "\x1B]0;isocline-title\x07";
    } else if (strcmp(scenario, "prompt_guard_newline_reset") == 0) {
        pre_prompt_output = "prefix line\n";
    } else if (strcmp(scenario, "prompt_guard_escape_then_visible") == 0) {
        pre_prompt_output = "\x1B[31mX\x1B[0m";
    } else if (strcmp(scenario, "prompt_guard_spaces_only") == 0) {
        pre_prompt_output = "   ";
    } else if (strcmp(scenario, "prompt_guard_controls_only") == 0) {
        pre_prompt_output = "\a\b\r\v\f";
    } else if (strcmp(scenario, "prompt_guard_carriage_return_only") == 0) {
        pre_prompt_output = "\r";
    } else if (strcmp(scenario, "prompt_guard_visible_then_carriage_return") == 0) {
        pre_prompt_output = "abc\r";
    } else if (strcmp(scenario, "prompt_guard_visible_then_carriage_return_clear") == 0) {
        pre_prompt_output = "abc\r\x1B[2K";
    } else if (strcmp(scenario, "prompt_guard_forced_visible_line_start") == 0) {
        ic_term_mark_line_visible(true);
        pre_prompt_output = "\r";
    } else if (strcmp(scenario, "prompt_guard_visible_then_newline") == 0) {
        pre_prompt_output = "abc\n";
    } else if (strcmp(scenario, "prompt_guard_newline_then_visible") == 0) {
        pre_prompt_output = "\nabc";
    } else if (strcmp(scenario, "prompt_guard_double_newline_reset") == 0) {
        pre_prompt_output = "abc\n\n";
    } else if (strcmp(scenario, "prompt_guard_escape_then_space") == 0) {
        pre_prompt_output = "\x1B[31m \x1B[0m";
    } else if (strcmp(scenario, "prompt_guard_escape_then_newline_then_visible") == 0) {
        pre_prompt_output = "\x1B[31m\nX\x1B[0m";
    } else if (strcmp(scenario, "prompt_guard_visible_then_newline_then_escape") == 0) {
        pre_prompt_output = "abc\n\x1B[31m\x1B[0m";
    } else if (strcmp(scenario, "prompt_guard_bracketed_toggle_only") == 0) {
        pre_prompt_output = "\x1B[?2004h\x1B[?2004l";
    } else if (strcmp(scenario, "prompt_guard_bracketed_toggle_then_tab") == 0) {
        pre_prompt_output = "\x1B[?2004h\t\x1B[?2004l";
    } else if (strcmp(scenario, "prompt_guard_utf8_visible") == 0) {
        pre_prompt_output = "\xE2\x82\xAC";
    } else if (strcmp(scenario, "prompt_guard_osc_then_space") == 0) {
        pre_prompt_output = "\x1B]0;x\x07 ";
    } else if (strcmp(scenario, "prompt_guard_region_marking_external_visible") == 0) {
        pre_prompt_output = "visible-before-prompt";
        external_pre_prompt_output = true;
    } else if (strncmp(scenario, "typeahead_", 10) == 0) {
        // Input is staged below, immediately before readline. Keeping these as
        // named scenarios makes each byte-level Return/Ctrl+J contract visible
        // in the PTY integration harness.
    } else {
        return 2;
    }

    if (pre_prompt_output != NULL && !external_pre_prompt_output) {
        ic_term_write(pre_prompt_output);
    }

    if (prompt_marker != NULL || continuation_prompt_marker != NULL) {
        ic_set_prompt_marker((prompt_marker != NULL ? prompt_marker : "> "),
                             continuation_prompt_marker);
    }
    (void)ic_enable_terminal_region_marking(region_marking);
    if (strcmp(scenario, "region_marking_transient_prompt_components") == 0) {
        ic_set_check_for_continuation_or_return_callback(apply_transient_prompt_on_submit, NULL);
    } else if (strcmp(scenario, "transient_prompt_multiline_clear") == 0) {
        ic_set_check_for_continuation_or_return_callback(apply_transient_prompt_on_submit,
                                                         "FINAL-TOP\nFINAL-MIDDLE\nfinal");
    } else if (strcmp(scenario, "multiline_max_lines_prompt_reset") == 0) {
        ic_set_check_for_continuation_or_return_callback(
            apply_transient_prompt_on_submit, "VIEWPORT-FINAL-TOP\nVIEWPORT-FINAL-MIDDLE\nfinal");
    }

    if (strncmp(scenario, "typeahead_", 10) == 0) {
        static const uint8_t return_submit[] = "queued command\r";
        static const uint8_t ctrl_j_then_live_return[] = "first\nsecond";
        static const uint8_t ctrl_j_then_queued_return[] = "first\nsecond\r";
        static const uint8_t leading_ctrl_j[] = "\nbody\r";
        static const uint8_t repeated_ctrl_j[] = "a\n\nb\r";
        static const uint8_t empty_return[] = "\r";
        static const uint8_t utf8_return[] = "caf\xC3\xA9 \xE2\x82\xAC\r";
        static const uint8_t edited_return[] = {'a', 'b', 'c', 0x7F, 'd', '\r'};
        static const uint8_t two_returns[] = "first\rsecond\r";
        static const uint8_t return_then_ctrl_j[] = "first\rsecond\nthird\r";
        static const uint8_t crlf_distinct[] = "first\r\nsecond\r";
        static const uint8_t long_return[] =
            "0123456789abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-"
            "typeahead-beyond-the-legacy-pushback-limit\r";

        const uint8_t* staged = NULL;
        size_t staged_len = 0;
        if (capture_typeahead_from_pty) {
            if (strcmp(scenario, "typeahead_capture_return") != 0 &&
                strcmp(scenario, "typeahead_capture_ctrl_j_then_return") != 0) {
                return 2;
            }
            (void)ic_enable_typeahead(true);
            emit_typeahead_capture_ready();
#if !defined(_WIN32)
            const struct timespec capture_delay = {.tv_sec = 0, .tv_nsec = 200000000L};
            (void)nanosleep(&capture_delay, NULL);
#endif
            if (!ic_typeahead_capture_available_input()) {
                return 9;
            }
        } else if (strcmp(scenario, "typeahead_return_submit") == 0) {
            staged = return_submit;
            staged_len = sizeof(return_submit) - 1;
        } else if (strcmp(scenario, "typeahead_ctrl_j_then_live_return") == 0) {
            staged = ctrl_j_then_live_return;
            staged_len = sizeof(ctrl_j_then_live_return) - 1;
        } else if (strcmp(scenario, "typeahead_ctrl_j_then_queued_return") == 0) {
            staged = ctrl_j_then_queued_return;
            staged_len = sizeof(ctrl_j_then_queued_return) - 1;
        } else if (strcmp(scenario, "typeahead_leading_ctrl_j") == 0) {
            staged = leading_ctrl_j;
            staged_len = sizeof(leading_ctrl_j) - 1;
        } else if (strcmp(scenario, "typeahead_repeated_ctrl_j") == 0) {
            staged = repeated_ctrl_j;
            staged_len = sizeof(repeated_ctrl_j) - 1;
        } else if (strcmp(scenario, "typeahead_empty_return") == 0) {
            staged = empty_return;
            staged_len = sizeof(empty_return) - 1;
        } else if (strcmp(scenario, "typeahead_utf8_return") == 0) {
            staged = utf8_return;
            staged_len = sizeof(utf8_return) - 1;
        } else if (strcmp(scenario, "typeahead_edited_return") == 0) {
            staged = edited_return;
            staged_len = sizeof(edited_return);
        } else if (strcmp(scenario, "typeahead_long_return") == 0) {
            staged = long_return;
            staged_len = sizeof(long_return) - 1;
        } else if (strcmp(scenario, "typeahead_two_returns") == 0) {
            staged = two_returns;
            staged_len = sizeof(two_returns) - 1;
            typeahead_two_readlines = true;
        } else if (strcmp(scenario, "typeahead_return_then_ctrl_j") == 0) {
            staged = return_then_ctrl_j;
            staged_len = sizeof(return_then_ctrl_j) - 1;
            typeahead_two_readlines = true;
        } else if (strcmp(scenario, "typeahead_crlf_distinct") == 0) {
            staged = crlf_distinct;
            staged_len = sizeof(crlf_distinct) - 1;
            typeahead_two_readlines = true;
        } else if (strcmp(scenario, "typeahead_interrupt_then_return") == 0) {
            (void)ic_enable_typeahead(true);
            typeahead_two_readlines = true;
        } else if (strcmp(scenario, "typeahead_chunked_return") == 0) {
            (void)ic_enable_typeahead(true);
            if (!ic_typeahead_ingest_raw_input((const uint8_t*)"chunked ", 8) ||
                !ic_typeahead_ingest_raw_input((const uint8_t*)"command", 7) ||
                !ic_typeahead_ingest_raw_input((const uint8_t*)"\r", 1)) {
                return 7;
            }
        } else {
            return 2;
        }

        if (staged != NULL) {
            (void)ic_enable_typeahead(true);
            if (!ic_typeahead_ingest_raw_input(staged, staged_len)) {
                return 7;
            }
        }
    }

    char* line = NULL;
    if (external_pre_prompt_output) {
        ic_set_prompt_eol_mark("\n%");
        char* first = ic_readline("seed", NULL, "seed\r");
        if (first == NULL) {
            return 4;
        }
        ic_free(first);

        ic_mark_command_start();
        (void)fwrite(pre_prompt_output, sizeof(char), strlen(pre_prompt_output), stdout);
        (void)fflush(stdout);
        ic_mark_command_finished(0);

        line = ic_readline(prompt_text, NULL, NULL);
    } else if (history_interactive_triplet) {
        char* first = ic_readline(prompt_text, NULL, NULL);
        if (first == NULL) {
            return 4;
        }
        ic_free(first);
        emit_readline_step_done();

        char* second = ic_readline(prompt_text, NULL, NULL);
        if (second == NULL) {
            return 4;
        }
        ic_free(second);
        emit_readline_step_done();

        line = ic_readline(prompt_text, NULL, NULL);
    } else if (history_sort_cycle_nonpersistent) {
        char* first = ic_readline(prompt_text, NULL, NULL);
        if (first == NULL) {
            return 4;
        }
        emit_readline_step_done();

        char* second = ic_readline(prompt_text, NULL, NULL);
        if (second == NULL) {
            ic_free(first);
            return 4;
        }

        char payload[512];
        (void)snprintf(payload, sizeof(payload), "%s|%s", first, second);
        ic_free(first);
        ic_free(second);
        emit_result(payload);
        return 0;
    } else if (typeahead_two_readlines) {
        char* first = ic_readline(prompt_text, NULL, NULL);
        if (first == NULL) {
            return 4;
        }
        char* second = ic_readline(prompt_text, NULL, NULL);
        if (second == NULL) {
            ic_free(first);
            return 4;
        }

        const size_t payload_len = strlen(first) + strlen(second) + 2;
        char* payload = (char*)malloc(payload_len);
        if (payload == NULL) {
            ic_free(first);
            ic_free(second);
            return 8;
        }
        (void)snprintf(payload, payload_len, "%s|%s", first, second);
        ic_free(first);
        ic_free(second);
        emit_result(payload);
        free(payload);
        return 0;
    } else {
        line = ic_readline(prompt_text, inline_right_text, initial_input);
    }
    if (line == NULL) {
        return 3;
    }

    if (region_marking) {
        ic_mark_command_start();
        ic_term_write("region-output");
        ic_mark_command_finished(7);
    }

    emit_result(line);
    ic_free(line);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 2;
    }
    return run_case(argv[1]);
}
