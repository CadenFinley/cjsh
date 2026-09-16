/*
  test_isocline_behavior.c

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

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "attr.h"
#include "bbcode.h"
#include "common.h"
#include "completions.h"
#include "editline_viewport.h"
#include "env.h"
#include "history.h"
#include "isocline.h"
#include "isocline_typeahead.h"
#include "keybindings.h"
#include "keycodes.h"
#include "prompt_line_replacement.h"
#include "stringbuf.h"
#include "term.h"
#include "tty.h"
#include "unicode.h"

static void expect_safe_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    (void)fputs(buffer, stderr);
    (void)fflush(stderr);
}

#define EXPECT_TRUE(condition, message)                                                        \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            expect_safe_log("EXPECT_TRUE failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
            return false;                                                                      \
        }                                                                                      \
    } while (0)

#define EXPECT_FALSE(condition, message) EXPECT_TRUE(!(condition), message)

#define EXPECT_STREQ(actual_expr, expected_expr, message)                                       \
    do {                                                                                        \
        const char* _actual = (actual_expr);                                                    \
        const char* _expected = (expected_expr);                                                \
        bool _match = false;                                                                    \
        if (_actual == NULL && _expected == NULL) {                                             \
            _match = true;                                                                      \
        } else if (_actual != NULL && _expected != NULL && strcmp(_actual, _expected) == 0) {   \
            _match = true;                                                                      \
        }                                                                                       \
        if (!_match) {                                                                          \
            expect_safe_log("EXPECT_STREQ failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
            expect_safe_log("  actual:   %s\n", _actual == NULL ? "(null)" : _actual);          \
            expect_safe_log("  expected: %s\n", _expected == NULL ? "(null)" : _expected);      \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

static ic_env_t* ensure_env(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        expect_safe_log("ic_get_env() returned NULL\n");
    }
    return env;
}

static alloc_t* test_allocator(void) {
    ic_env_t* env = ensure_env();
    return (env == NULL ? NULL : env->mem);
}

static const char* g_observed_completion_input = NULL;
static long g_observed_completion_cursor = -1;

static void sample_completion_builder(ic_completion_env_t* cenv, const char* prefix) {
    g_observed_completion_input = ic_completion_input(cenv, &g_observed_completion_cursor);
    const char* effective_prefix = (prefix == NULL) ? "" : prefix;
    if (effective_prefix[0] != '\0' && effective_prefix[0] != 'a') {
        return;
    }
    (void)ic_add_completion_prim_with_source(cenv, "alpha", "[warn]alpha", "first", "history", 1,
                                             0);
    (void)ic_add_completion_prim_with_source(cenv, "alphabet", NULL, NULL, "history", 1, 0);
    (void)ic_add_completion_prim_with_source(cenv, "alpine", "[note]alpine", "mountain", "files", 1,
                                             0);
}

static stringbuf_t* new_stringbuf(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return NULL;
    }
    return sbuf_new(mem);
}

static void reset_typeahead_test_state(bool enabled) {
    ic_set_typeahead_capture_allowed_callback(NULL, NULL);
    (void)ic_enable_typeahead(enabled);
    ic_typeahead_clear();
}

static bool stub_continuation_checker(const char* buffer, void* arg) {
    bool arg_valid = (arg != NULL);
    bool buffer_valid = (buffer != NULL);
    return arg_valid && (buffer_valid || arg_valid);
}

static const char* stub_status_message(const char* input_buffer, void* arg) {
    ic_unused(input_buffer);
    return (arg != NULL ? "status" : NULL);
}

static int g_typeahead_capture_gate_calls = 0;
static bool g_typeahead_capture_gate_result = true;
static void* g_typeahead_capture_gate_last_arg = NULL;

static bool stub_typeahead_capture_allowed(void* arg) {
    g_typeahead_capture_gate_calls++;
    g_typeahead_capture_gate_last_arg = arg;
    return g_typeahead_capture_gate_result;
}

static bool g_command_palette_handler_called = false;
static const char* g_command_palette_handler_last_id = NULL;
static void* g_command_palette_handler_last_arg = NULL;

static bool stub_command_palette_handler(const ic_command_palette_entry_t* entry, void* arg) {
    g_command_palette_handler_called = true;
    g_command_palette_handler_last_id = (entry != NULL ? entry->id : NULL);
    g_command_palette_handler_last_arg = arg;
    return (entry != NULL && entry->id != NULL && entry->id[0] != '\0');
}

static bool test_readline_disposition_name_mappings(void) {
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_SUBMIT), "submit",
                 "submit disposition name mismatch");
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_INTERRUPT), "interrupt",
                 "interrupt disposition name mismatch");
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_EOF), "eof",
                 "eof disposition name mismatch");
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_STOP), "stop",
                 "stop disposition name mismatch");
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_IDLE), "idle",
                 "idle disposition name mismatch");
    EXPECT_STREQ(ic_readline_disposition_name(IC_READLINE_DISPOSITION_ERROR), "error",
                 "error disposition name mismatch");
    // Exercise the public API's fallback for an invalid enum value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_STREQ(ic_readline_disposition_name((ic_readline_disposition_t)999), "error",
                 "unknown disposition should map to error");
    return true;
}

static bool test_multiline_toggle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->singleline_only = true;
    bool was_enabled = ic_enable_multiline(true);
    EXPECT_FALSE(was_enabled, "multiline should report previously disabled state");
    EXPECT_FALSE(env->singleline_only, "enabling multiline should clear singleline_only flag");

    bool was_enabled_before_disable = ic_enable_multiline(false);
    EXPECT_TRUE(was_enabled_before_disable,
                "disabling multiline should report it was previously enabled");
    EXPECT_TRUE(env->singleline_only, "disabling multiline should set singleline_only flag");

    // Restore default state for later tests
    (void)ic_enable_multiline(true);
    return true;
}

static bool test_multiline_continuation_retention_toggle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->retain_multiline_continuation = false;
    EXPECT_FALSE(ic_enable_multiline_continuation_retention(true),
                 "continuation retention should report previously disabled state");
    EXPECT_TRUE(env->retain_multiline_continuation,
                "enabling continuation retention should set the environment flag");

    EXPECT_TRUE(ic_enable_multiline_continuation_retention(false),
                "disabling continuation retention should report previously enabled state");
    EXPECT_FALSE(env->retain_multiline_continuation,
                 "disabling continuation retention should clear the environment flag");
    return true;
}

static bool test_line_number_modes(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->show_line_numbers = true;
    env->relative_line_numbers = false;

    bool prev_state = ic_enable_line_numbers(false);
    EXPECT_TRUE(prev_state, "ic_enable_line_numbers should return previous enabled state");
    EXPECT_FALSE(env->show_line_numbers, "line numbers should be disabled");
    EXPECT_FALSE(env->relative_line_numbers, "disabling line numbers should clear relative flag");

    env->show_line_numbers = false;
    env->relative_line_numbers = false;

    bool prev_relative = ic_enable_relative_line_numbers(true);
    EXPECT_FALSE(prev_relative, "ic_enable_relative_line_numbers should report previous state");
    EXPECT_TRUE(env->relative_line_numbers, "relative line numbers should now be enabled");
    EXPECT_TRUE(env->show_line_numbers,
                "enabling relative numbering should force absolute line numbers on");

    bool prev_relative_disable = ic_enable_relative_line_numbers(false);
    EXPECT_TRUE(prev_relative_disable,
                "disabling relative numbering should report it was previously enabled");
    EXPECT_FALSE(env->relative_line_numbers, "relative line numbers should be disabled");

    return true;
}

static bool test_line_number_continuation_prompt_toggle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->allow_line_numbers_with_continuation_prompt = false;
    bool prev = ic_enable_line_numbers_with_continuation_prompt(true);
    EXPECT_FALSE(
        prev,
        "enabling line numbers with continuation prompts should report previously disabled state");
    EXPECT_TRUE(env->allow_line_numbers_with_continuation_prompt,
                "environment flag should mirror requested enablement");
    EXPECT_TRUE(ic_line_numbers_with_continuation_prompt_are_enabled(),
                "getter should report enabled state");

    bool prev_disable = ic_enable_line_numbers_with_continuation_prompt(false);
    EXPECT_TRUE(
        prev_disable,
        "disabling line numbers with continuation prompts should report prior enabled state");
    EXPECT_FALSE(env->allow_line_numbers_with_continuation_prompt,
                 "environment flag should be cleared after disabling");
    EXPECT_FALSE(ic_line_numbers_with_continuation_prompt_are_enabled(),
                 "getter should report disabled state");

    return true;
}

static bool test_line_number_prompt_replacement_toggle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->replace_prompt_line_with_line_number = false;
    bool prev = ic_enable_line_number_prompt_replacement(true);
    EXPECT_FALSE(prev, "enabling prompt line replacement should report previously disabled state");
    EXPECT_TRUE(env->replace_prompt_line_with_line_number,
                "environment flag should mirror requested enablement");
    EXPECT_TRUE(ic_line_number_prompt_replacement_is_enabled(),
                "getter should report enabled state");

    bool prev_disable = ic_enable_line_number_prompt_replacement(false);
    EXPECT_TRUE(prev_disable,
                "disabling prompt line replacement should report prior enabled state");
    EXPECT_FALSE(env->replace_prompt_line_with_line_number,
                 "environment flag should be cleared after disabling");
    EXPECT_FALSE(ic_line_number_prompt_replacement_is_enabled(),
                 "getter should report disabled state");

    return true;
}

static bool test_prompt_line_replacement_requires_content(void) {
    ic_prompt_line_replacement_state_t predicate = {
        .replace_prompt_line_with_line_number = true,
        .prompt_has_prefix_lines = true,
        .prompt_begins_with_newline = false,
        .line_numbers_enabled = true,
        .input_has_content = true,
    };

    EXPECT_TRUE(ic_prompt_line_replacement_should_activate(&predicate),
                "predicate should activate when buffer contains input");

    predicate.input_has_content = false;
    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(&predicate),
                 "predicate should keep the prompt visible when the buffer is empty");

    return true;
}

static bool test_line_wrap_marker(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }
#ifdef __APPLE__
    const char* default_marker = "↵";
#else
    const char* default_marker = "←";
#endif
    EXPECT_STREQ(ic_get_line_wrap_marker(), default_marker, "default wrap marker mismatch");
    EXPECT_TRUE(ic_set_line_wrap_marker(""), "empty marker should disable wrap indicators");
    EXPECT_STREQ(ic_get_line_wrap_marker(), "", "disabled marker should be empty");
    EXPECT_TRUE(env->line_wrap_marker_width == 0, "empty marker should not reserve any columns");

    char custom_marker[] = ">";
    EXPECT_TRUE(ic_set_line_wrap_marker(custom_marker), "ASCII marker should be accepted");
    custom_marker[0] = '!';
    EXPECT_STREQ(ic_get_line_wrap_marker(), ">", "setter should copy the marker");
    EXPECT_TRUE(ic_set_line_wrap_marker(ic_get_line_wrap_marker()),
                "setter should accept its own getter's result");
    EXPECT_STREQ(ic_get_line_wrap_marker(), ">", "self-assignment should preserve the marker");

    const char* markers[] = {"é", "↪", "界", "😀", "[", "'", " ", "0", "1"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        EXPECT_TRUE(ic_set_line_wrap_marker(markers[i]),
                    "one Unicode character should be accepted");
        EXPECT_STREQ(ic_get_line_wrap_marker(), markers[i], "marker should round-trip verbatim");
    }
    EXPECT_TRUE(ic_set_line_wrap_marker("界"), "wide character should be accepted");
    EXPECT_TRUE(env->line_wrap_marker_width == 2, "wide marker should reserve two columns");

    const char* invalid[] = {
        "on",           "off",  "ab",       "↪↪",       "e\xCC\x81",    "\n",
        "\r",           "\t",   "\x1B",     "\x7F",     "\xC2\x85",     "\xCC\x81",
        "\xE2\x80\x8D", "\xFF", "\xC0\xAF", "\xE2\x86", "\xED\xA0\x80", "\xF4\x90\x80\x80",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        EXPECT_FALSE(ic_set_line_wrap_marker(invalid[i]), "invalid marker should be rejected");
        EXPECT_STREQ(ic_get_line_wrap_marker(), "界", "rejection should preserve the marker");
        EXPECT_TRUE(env->line_wrap_marker_width == 2, "rejection should preserve its width");
    }
    EXPECT_TRUE(ic_set_line_wrap_marker(NULL), "NULL should restore the default marker");
    EXPECT_STREQ(ic_get_line_wrap_marker(), default_marker, "reset should restore default marker");
    EXPECT_TRUE(env->line_wrap_marker_width == 1, "default marker should reserve one column");
    return true;
}

static bool test_visible_whitespace_marker(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->show_whitespace_characters = false;
    ic_set_whitespace_marker(NULL);

    const char* default_marker = "\xC2\xB7";  // UTF-8 middle dot
    EXPECT_STREQ(ic_get_whitespace_marker(), default_marker, "default whitespace marker mismatch");

    bool prev = ic_enable_visible_whitespace(true);
    EXPECT_FALSE(prev, "visible whitespace should report previously disabled state");
    EXPECT_TRUE(env->show_whitespace_characters,
                "visible whitespace flag should be enabled after calling API");

    const char* custom_marker = "<·>";
    ic_set_whitespace_marker(custom_marker);
    EXPECT_STREQ(ic_get_whitespace_marker(), custom_marker,
                 "custom whitespace marker should be applied verbatim");

    ic_set_whitespace_marker(NULL);
    EXPECT_STREQ(ic_get_whitespace_marker(), default_marker,
                 "resetting whitespace marker should restore default symbol");

    (void)ic_enable_visible_whitespace(false);
    return true;
}

static bool test_multiline_start_line_count_clamp(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->multiline_start_line_count = 4;

    size_t previous = ic_set_multiline_start_line_count(0);
    EXPECT_TRUE(previous == 4, "ic_set_multiline_start_line_count should return previous value");
    EXPECT_TRUE(env->multiline_start_line_count == 1,
                "multiline start line count should clamp to minimum of 1");

    previous = ic_set_multiline_start_line_count(300);
    EXPECT_TRUE(previous == 1,
                "ic_set_multiline_start_line_count should report most recent stored value");
    EXPECT_TRUE(env->multiline_start_line_count == 256,
                "multiline start line count should clamp to maximum of 256");

    previous = ic_set_multiline_start_line_count(3);
    EXPECT_TRUE(previous == 256, "previous value should reflect clamped maximum");
    EXPECT_TRUE(env->multiline_start_line_count == 3,
                "multiline start line count should accept values within the allowed range");

    return true;
}

static bool test_multiline_max_line_count_defaults_and_clamps(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    EXPECT_TRUE(ic_get_multiline_max_line_count() == 15,
                "multiline viewport should default to 15 visible rows");

    size_t previous = ic_set_multiline_max_line_count(0);
    EXPECT_TRUE(previous == 15, "multiline maximum setter should return the previous value");
    EXPECT_TRUE(env->multiline_max_line_count == 1,
                "multiline maximum line count should clamp to a minimum of 1");

    previous = ic_set_multiline_max_line_count(300);
    EXPECT_TRUE(previous == 1,
                "multiline maximum setter should report the most recent stored value");
    EXPECT_TRUE(env->multiline_max_line_count == 256,
                "multiline maximum line count should clamp to 256");

    previous = ic_set_multiline_max_line_count(15);
    EXPECT_TRUE(previous == 256, "previous value should reflect the clamped maximum");
    EXPECT_TRUE(ic_get_multiline_max_line_count() == 15,
                "multiline maximum line count should accept the default value");

    return true;
}

static bool test_multiline_bottom_line_count_defaults_and_clamps(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    EXPECT_TRUE(ic_get_multiline_bottom_line_count() == 3,
                "multiline viewport should default to three content rows below the cursor");

    size_t previous = ic_set_multiline_bottom_line_count(0);
    EXPECT_TRUE(previous == 3,
                "ic_set_multiline_bottom_line_count should return the previous value");
    EXPECT_TRUE(env->multiline_bottom_line_count == 0,
                "multiline bottom line count should allow zero");

    previous = ic_set_multiline_bottom_line_count(300);
    EXPECT_TRUE(previous == 0,
                "multiline bottom line setter should report the most recent stored value");
    EXPECT_TRUE(env->multiline_bottom_line_count == 256,
                "multiline bottom line count should clamp to 256");

    previous = ic_set_multiline_bottom_line_count(3);
    EXPECT_TRUE(previous == 256, "previous value should reflect the clamped maximum");
    EXPECT_TRUE(ic_get_multiline_bottom_line_count() == 3,
                "multiline bottom line count should accept the default value");

    return true;
}

static bool test_menu_max_line_count_defaults_and_clamps(void) {
    EXPECT_TRUE(ensure_env() != NULL, "menu configuration requires an environment");
    size_t (*setters[])(size_t) = {ic_set_completion_menu_max_line_count,
                                  ic_set_history_menu_max_line_count,
                                  ic_set_command_palette_max_line_count,
                                  ic_set_custom_menu_max_line_count};
    size_t (*getters[])(void) = {ic_get_completion_menu_max_line_count,
                               ic_get_history_menu_max_line_count,
                               ic_get_command_palette_max_line_count,
                               ic_get_custom_menu_max_line_count};
    const size_t defaults[] = {15, 30, 30, 30};
    for (size_t i = 0; i < 4; i++) {
        EXPECT_TRUE(getters[i]() == defaults[i], "each menu should use its own default row limit");
        EXPECT_TRUE(setters[i](0) == defaults[i], "setter should return the previous limit");
        EXPECT_TRUE(getters[i]() == 1, "zero should clamp to one row");
        EXPECT_TRUE(setters[i]((size_t)-1) == 1, "setter should accept large unsigned values");
        EXPECT_TRUE(getters[i]() == 256, "maximum should clamp to 256 rows");
        EXPECT_TRUE(setters[i](75) == 256 && getters[i]() == 75,
                    "maximum should allow limits above the default");
        for (size_t j = 0; j < 4; j++) {
            EXPECT_TRUE(getters[j]() == (i == j ? 75 : defaults[j]),
                        "changing one menu must not change any other menu");
        }
        (void)setters[i](defaults[i]);
    }
    EXPECT_TRUE(
        ic_get_multiline_max_line_count() == 15 && ic_get_multiline_bottom_line_count() == 3,
        "menu maximum should not change multiline height or the scroll margin");
    return true;
}

static bool test_multiline_viewport_layout(void) {
    editline_viewport_t viewport = editline_viewport_for(20, 0, 19, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 5 && viewport.input_row_count == 15,
                "viewport should show the final 15 input rows at end of the buffer");
    EXPECT_TRUE(viewport.extra_row_count == 0,
                "viewport without helpers should not reserve extra rows");

    viewport = editline_viewport_for(20, 0, 0, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 0 && viewport.input_row_count == 15,
                "viewport should keep the first input row visible at buffer start");

    viewport = editline_viewport_for(20, 5, 19, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 5 && viewport.input_row_count == 15,
                "helper rows should coexist with the configured input viewport when space allows");
    EXPECT_TRUE(viewport.extra_row_count == 5,
                "all helper rows should remain visible when they fit below the input viewport");

    viewport = editline_viewport_for(20, 10, 19, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 6 && viewport.input_row_count == 14,
                "input viewport should yield screen space when helper rows fill the terminal");
    EXPECT_TRUE(viewport.extra_row_count == 10,
                "already-windowed helper rows should be preserved where possible");

    viewport = editline_viewport_for(20, 0, 19, 10, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 10 && viewport.input_row_count == 10,
                "terminal height should bound the configured input viewport");

    return true;
}

static bool test_multiline_viewport_bottom_content_rows(void) {
    editline_viewport_t viewport = editline_viewport_for(20, 0, 14, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 3 && viewport.input_row_count == 15,
                "viewport should retain three existing rows below the cursor");

    viewport = editline_viewport_for(20, 0, 16, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 5 && viewport.input_row_count == 15,
                "viewport should stop at the final content row without adding blank rows");

    viewport = editline_viewport_for(20, 0, 19, 24, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 5 && viewport.input_row_count == 15,
                "viewport should keep the final content-filled window at end of input");

    viewport = editline_viewport_for(20, 0, 14, 24, 15, 0, 0);
    EXPECT_TRUE(viewport.input_first_row == 0 && viewport.input_row_count == 15,
                "zero bottom rows should allow the cursor to reach the viewport bottom");

    viewport = editline_viewport_for(20, 0, 5, 5, 20, 20, 0);
    EXPECT_TRUE(viewport.input_first_row == 3 && viewport.input_row_count == 5,
                "scroll margin should be bounded symmetrically by the visible viewport");

    viewport = editline_viewport_for(20, 4, 14, 15, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 7 && viewport.input_row_count == 11 &&
                    viewport.extra_row_count == 4,
                "bottom row preference should use the input space left after helper rows");

    return true;
}

static bool test_multiline_viewport_symmetric_scroll_margin(void) {
    editline_viewport_t viewport = editline_viewport_for(30, 0, 20, 15, 15, 3, 0);
    EXPECT_TRUE(viewport.input_first_row == 9,
                "viewport should place a downward-moving cursor above the bottom margin");

    viewport = editline_viewport_for(30, 0, 19, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 9,
                "viewport should stay fixed while the cursor moves upward within its margins");

    viewport = editline_viewport_for(30, 0, 12, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 9,
                "viewport should remain fixed when the cursor reaches the top margin");

    viewport = editline_viewport_for(30, 0, 11, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 8,
                "viewport should scroll only after the cursor crosses the top margin");

    viewport = editline_viewport_for(30, 0, 12, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 8,
                "viewport should stay fixed while the cursor moves downward within its margins");

    viewport = editline_viewport_for(30, 0, 19, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 8,
                "viewport should remain fixed when the cursor reaches the bottom margin");

    viewport = editline_viewport_for(30, 0, 20, 15, 15, 3, viewport.input_first_row);
    EXPECT_TRUE(viewport.input_first_row == 9,
                "viewport should scroll only after the cursor crosses the bottom margin");

    return true;
}

static bool test_editline_buffer_api_without_editor(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->current_editor = NULL;
    EXPECT_FALSE(ic_set_buffer("demo"), "setting buffer without editor should fail");
    EXPECT_TRUE(ic_get_buffer() == NULL, "get buffer should return NULL without editor");

    size_t pos = 42;
    EXPECT_FALSE(ic_get_cursor_pos(&pos), "cursor query should fail without editor");
    EXPECT_TRUE(pos == 42, "cursor output argument should remain unchanged on failure");

    EXPECT_FALSE(ic_set_cursor_pos(1), "cursor set should fail without editor");
    EXPECT_FALSE(ic_request_submit(), "submit request should fail without editor");
    EXPECT_FALSE(ic_execute_key_action(IC_KEY_ACTION_CURSOR_UP),
                 "editor action should fail without editor");
    EXPECT_FALSE(ic_current_loop_reset("buf", "prompt", "inline"),
                 "loop reset should fail without editor");

    return true;
}

static bool test_continuation_callback_registration(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->continuation_check_callback = NULL;
    env->continuation_check_arg = NULL;

    ic_set_check_for_continuation_or_return_callback(stub_continuation_checker, (void*)0x1);
    EXPECT_TRUE(env->continuation_check_callback == stub_continuation_checker,
                "setter should store continuation callback pointer");
    EXPECT_TRUE(env->continuation_check_arg == (void*)0x1,
                "setter should store continuation callback argument");

    ic_set_check_for_continuation_or_return_callback(NULL, NULL);
    EXPECT_TRUE(env->continuation_check_callback == NULL,
                "clearing continuation callback should reset pointer");
    EXPECT_TRUE(env->continuation_check_arg == NULL,
                "clearing continuation callback should reset argument");

    return true;
}

static bool test_completion_generation_and_apply(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_completer_fun_t* prev_fun = NULL;
    void* prev_arg = NULL;
    completions_get_completer(env->completions, &prev_fun, &prev_arg);
    completions_set_completer(env->completions, &sample_completion_builder, NULL);

    g_observed_completion_input = NULL;
    g_observed_completion_cursor = -1;
    ssize_t produced = completions_generate(env, env->completions, "a", 1, 8);
    EXPECT_TRUE(produced == 3, "stub completer should generate three entries");
    EXPECT_STREQ(g_observed_completion_input, "a",
                 "completion callbacks should expose the complete input buffer");
    EXPECT_TRUE(g_observed_completion_cursor == 1,
                "completion callbacks should expose the cursor position");
    long null_cursor = 7;
    EXPECT_TRUE(ic_completion_input(NULL, &null_cursor) == NULL,
                "completion input should reject a null environment");
    EXPECT_TRUE(null_cursor == 7,
                "a rejected completion input query should leave its cursor output unchanged");
    completions_sort(env->completions);

    const char* help = NULL;
    const char* display0 = completions_get_display(env->completions, 0, &help);
    EXPECT_STREQ(display0, "\\[warn]alpha", "bbcode brackets should be escaped in display");
    EXPECT_STREQ(help, "first", "help metadata should be preserved");

    bool found_alphabet = false;
    bool found_alpine = false;
    const char* alpine_source = NULL;
    for (ssize_t i = 0; i < produced; ++i) {
        const char* replacement = completions_get_replacement(env->completions, i);
        if (replacement == NULL) {
            continue;
        }
        if (strcmp(replacement, "alphabet") == 0) {
            found_alphabet = true;
        }
        if (strcmp(replacement, "alpine") == 0) {
            found_alpine = true;
            alpine_source = completions_get_source(env->completions, i);
        }
    }
    EXPECT_TRUE(found_alphabet && found_alpine,
                "completions should contain both 'alphabet' and 'alpine'");

    const char* hint0 = completions_get_hint(env->completions, 0, &help);
    EXPECT_STREQ(hint0, "lpha", "hint should expose remaining suffix after delete_before");

    EXPECT_TRUE(alpine_source != NULL && strcmp(alpine_source, "files") == 0,
                "source metadata should be recorded for alpine completion");

    const char* replacement_for_range = NULL;
    ssize_t replacement_start = -1;
    ssize_t delete_after = -1;
    EXPECT_TRUE(completions_get_apply_range(env->completions, 0, "a", 1, &replacement_for_range,
                                            &replacement_start, &delete_after),
                "completion apply range should be available");
    EXPECT_STREQ(replacement_for_range, "alpha",
                 "apply range should expose the inserted replacement");
    EXPECT_TRUE(replacement_start == 0, "apply range should account for delete_before");
    EXPECT_TRUE(delete_after == 0, "apply range should account for delete_after");

    stringbuf_t* sb = new_stringbuf();
    if (sb == NULL) {
        return false;
    }
    sbuf_replace(sb, "a");
    ssize_t new_pos = completions_apply(env->completions, 0, sb, 1);
    EXPECT_TRUE(new_pos > 1, "completion apply should advance cursor");
    EXPECT_STREQ(sbuf_string(sb), "alpha", "applying first completion should replace buffer");

    sbuf_replace(sb, "a");
    ssize_t prefix_pos = completions_apply_longest_prefix(env->completions, sb, 1);
    EXPECT_TRUE(prefix_pos >= 2, "longest prefix should extend beyond initial prefix");
    EXPECT_TRUE(strncmp(sbuf_string(sb), "al", 2) == 0,
                "longest common prefix across completions should start with 'al'");

    sbuf_free(sb);
    completions_clear(env->completions);
    completions_set_completer(env->completions, prev_fun, prev_arg);
    return true;
}

static bool test_history_dedup_snapshot(void) {
    ic_env_t* env = ensure_env();
    alloc_t* mem = test_allocator();
    if (env == NULL || mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_behavior.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    (void)history_enable_duplicates(history, false);
    EXPECT_TRUE(!history_push(history, NULL),
                "invalid input should not disable history persistence");
    EXPECT_TRUE(history_push(history, "echo hi"), "initial history push should succeed");
    EXPECT_TRUE(history_push(history, "echo hi"), "duplicate push should rewrite last entry");

    (void)history_enable_duplicates(history, true);
    const ic_history_metadata_t duplicate_meta[] = {
        {"exit_code", "7"},
    };
    EXPECT_TRUE(history_push_with_metadata(history, "echo hi", duplicate_meta,
                                           sizeof(duplicate_meta) / sizeof(duplicate_meta[0])),
                "duplicates should be kept once enabled");
    EXPECT_TRUE(history_push(history, "printf bye"), "new unique entry should append");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, false), "snapshot should load from file");
    EXPECT_TRUE(snap.count == 3, "snapshot should contain three entries");

    bool found_printf = false;
    ssize_t echo_instances = 0;
    for (ssize_t i = 0; i < snap.count; ++i) {
        const history_entry_t* entry = history_snapshot_get(&snap, i);
        if (entry == NULL) {
            continue;
        }
        if (strcmp(entry->command, "printf bye") == 0) {
            found_printf = true;
        }
        if (strcmp(entry->command, "echo hi") == 0) {
            echo_instances++;
        }
    }
    EXPECT_TRUE(found_printf, "history snapshot should contain the printf entry");
    EXPECT_TRUE(echo_instances >= 2, "history snapshot should retain duplicate echo entries");

    ssize_t search_idx = -1;
    EXPECT_TRUE(history_search_prefix(history, 0, "printf", true, &search_idx),
                "prefix search should find most recent match");
    EXPECT_TRUE(search_idx >= 0, "search index should be non-negative");
    const char* found_command = history_get(history, search_idx);
    EXPECT_TRUE(found_command != NULL && strcmp(found_command, "printf bye") == 0,
                "prefix search should reference the printf entry");

    history_snapshot_free(history, &snap);
    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_snapshot_search_consistency(void) {
    alloc_t* mem = test_allocator();
    EXPECT_TRUE(mem != NULL, "history test allocator should exist");
    history_t* reader = history_new(mem);
    history_t* writer = history_new(mem);
    EXPECT_TRUE(reader != NULL && writer != NULL, "history handles should be allocated");
    const char* path = "./isocline_history_snapshot_search.log";
    (void)remove(path);
    history_load_from(reader, path, 32);
    history_load_from(writer, path, 32);
    EXPECT_TRUE(history_push(writer, "alpha"), "initial history entry should persist");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(reader, &snap, true), "snapshot should load");
    EXPECT_TRUE(history_snapshot_is_current(reader, &snap),
                "unchanged history should stay current");
    EXPECT_TRUE(history_push(writer, "beta"), "another session should append history");
    EXPECT_FALSE(history_snapshot_is_current(reader, &snap),
                 "file replacement should invalidate snapshot");

    history_match_t matches[8];
    ssize_t count = 0;
    EXPECT_TRUE(
        history_snapshot_fuzzy_search(reader, &snap, "alpha", matches, 8, &count, NULL, true),
        "search should use the retained snapshot");
    EXPECT_TRUE(count == 1, "snapshot should contain one matching entry");
    EXPECT_STREQ(history_snapshot_get(&snap, matches[0].hidx)->command, "alpha",
                 "match indices and rendered entries must use the same snapshot");
    EXPECT_FALSE(
        history_snapshot_fuzzy_search(reader, &snap, "beta", matches, 8, &count, NULL, true),
        "search must not independently reload newer entries");
    EXPECT_TRUE(history_snapshot_load(reader, &snap, true), "changed history should reload");
    EXPECT_TRUE(history_snapshot_is_current(reader, &snap), "reloaded snapshot should be current");
    EXPECT_TRUE(
        history_snapshot_fuzzy_search(reader, &snap, "beta", matches, 8, &count, NULL, true),
        "reloaded snapshot should expose another session's entry");
    EXPECT_STREQ(history_snapshot_get(&snap, matches[0].hidx)->command, "beta",
                 "new match index should resolve to the new entry");
    history_snapshot_free(reader, &snap);
    history_free(reader);
    history_free(writer);
    (void)remove(path);
    return true;
}

static bool test_history_frequency_metadata_tracking(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_frequency.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "echo hi"),
                "first history entry should be stored with frequency metadata");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, false),
                "snapshot should load the initial frequency-tracked history entry");
    EXPECT_TRUE(history_snapshot_count(&snap) == 1,
                "first push should produce exactly one stored history entry");

    const history_entry_t* newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "snapshot should expose the stored history entry");
    EXPECT_STREQ(history_entry_get_metadata(newest, "frequency"), "1",
                 "new history entries should start with a frequency of one");
    EXPECT_TRUE(history_entry_get_metadata(newest, "timestamp") != NULL,
                "new history entries without user metadata should receive a timestamp");
    history_snapshot_free(history, &snap);

    EXPECT_TRUE(history_push(history, "echo hi"),
                "duplicate push should still succeed when duplicates are disabled");
    EXPECT_TRUE(history_snapshot_load(history, &snap, false),
                "snapshot should reload rewritten duplicate-suppressed history entry");
    EXPECT_TRUE(history_snapshot_count(&snap) == 1,
                "duplicate suppression should keep only one stored command entry");

    newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "rewritten duplicate entry should remain readable");
    EXPECT_STREQ(history_entry_get_metadata(newest, "frequency"), "2",
                 "duplicate-suppressed pushes should increment the stored frequency");
    EXPECT_STREQ(history_get(history, 0), "echo hi",
                 "history_get should still return the latest duplicate-suppressed command");
    history_snapshot_free(history, &snap);

    const ic_history_metadata_t latest_meta[] = {
        {"code", "7"},
    };
    EXPECT_TRUE(history_push_with_metadata(history, "echo hi", latest_meta,
                                           sizeof(latest_meta) / sizeof(latest_meta[0])),
                "duplicate rewrite with metadata should still succeed");
    EXPECT_TRUE(history_snapshot_load(history, &snap, false),
                "snapshot should load duplicate-suppressed entry after metadata update");

    newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "latest duplicate-suppressed entry should be available");
    EXPECT_STREQ(history_entry_get_metadata(newest, "code"), "7",
                 "new metadata should still be attached to the rewritten history entry");
    EXPECT_TRUE(history_entry_get_metadata(newest, "frequency") == NULL,
                "user metadata should disable the default frequency");
    EXPECT_TRUE(history_entry_get_metadata(newest, "timestamp") == NULL,
                "user metadata should disable the default timestamp");
    history_snapshot_free(history, &snap);

    const ic_history_metadata_t explicit_default_meta[] = {
        {"tag", "custom"},
        {"frequency", "9"},
        {"timestamp", "123"},
    };
    EXPECT_TRUE(history_push_with_metadata(
                    history, "custom metadata", explicit_default_meta,
                    sizeof(explicit_default_meta) / sizeof(explicit_default_meta[0])),
                "history should accept explicit frequency and timestamp values");
    EXPECT_TRUE(history_snapshot_load(history, &snap, false),
                "snapshot should load explicit user metadata");

    newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "explicit user metadata entry should be available");
    EXPECT_STREQ(history_entry_get_metadata(newest, "frequency"), "9",
                 "a nonzero user frequency should be preserved");
    EXPECT_STREQ(history_entry_get_metadata(newest, "timestamp"), "123",
                 "a nonzero user timestamp should be preserved");
    history_snapshot_free(history, &snap);

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_frequency_metadata_interactive_flow(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_frequency_interactive.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    const ic_history_metadata_t metadata[] = {
        {"timestamp", "0"},
        {"frequency", "0"},
        {"code", "0"},
        {"ms", "42"},
    };

    for (int expected_frequency = 1; expected_frequency <= 3; ++expected_frequency) {
        EXPECT_TRUE(history_push(history, ""),
                    "interactive input should stage a temporary history entry");
        EXPECT_TRUE(history_update(history, "ls"),
                    "accepting interactive input should rewrite the staged entry");

        history_remove_last(history);
        EXPECT_TRUE(history_push_with_metadata(history, "ls", metadata,
                                               sizeof(metadata) / sizeof(metadata[0])),
                    "executed interactive command should be stored with metadata");

        history_snapshot_t snap = {0};
        EXPECT_TRUE(history_snapshot_load(history, &snap, false),
                    "interactive execution snapshot should load from history");
        EXPECT_TRUE(history_snapshot_count(&snap) == 1,
                    "interactive duplicate suppression should keep one ls entry");

        const history_entry_t* newest = history_snapshot_get(&snap, 0);
        EXPECT_TRUE(newest != NULL, "interactive flow should keep the latest history entry");

        char expected_buf[16];
        (void)snprintf(expected_buf, sizeof(expected_buf), "%d", expected_frequency);
        EXPECT_STREQ(history_entry_get_metadata(newest, "frequency"), expected_buf,
                     "a zero user frequency should use isocline's frequency tracking");
        const char* timestamp = history_entry_get_metadata(newest, "timestamp");
        EXPECT_TRUE(timestamp != NULL && strcmp(timestamp, "0") != 0,
                    "a zero user timestamp should use isocline's current timestamp");
        EXPECT_STREQ(history_entry_get_metadata(newest, "code"), "0",
                     "isocline should preserve CJ's Shell exit-code metadata");
        EXPECT_STREQ(history_entry_get_metadata(newest, "ms"), "42",
                     "isocline should preserve CJ's Shell elapsed-time metadata");
        history_snapshot_free(history, &snap);
    }

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_snapshot_dedup_keeps_latest_entry(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_dedup_latest.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    const ic_history_metadata_t executed_meta[] = {
        {"code", "7"},
    };

    EXPECT_TRUE(history_push_with_metadata(history, "echo hi", executed_meta,
                                           sizeof(executed_meta) / sizeof(executed_meta[0])),
                "executed history entry should be stored with exit-code metadata");
    EXPECT_TRUE(history_push(history, ""),
                "interactive staging should append a transient blank history entry");
    EXPECT_TRUE(history_update(history, "echo hi"),
                "interactive staging update should rewrite the transient entry to the buffer");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, true),
                "deduplicated snapshot should load successfully with staged duplicates present");
    EXPECT_TRUE(
        history_snapshot_count(&snap) == 1,
        "deduplicated snapshot should collapse staged and executed duplicates into one entry");

    const history_entry_t* newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "deduplicated snapshot should expose the surviving history entry");
    EXPECT_TRUE(history_entry_get_metadata(newest, "code") == NULL,
                "deduplicated snapshot should preserve the latest entry");
    history_snapshot_free(history, &snap);

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_dedup_order_and_metadata(void) {
    alloc_t* mem = test_allocator();
    EXPECT_TRUE(mem != NULL, "history test allocator should exist");
    history_t* history = history_new(mem);
    EXPECT_TRUE(history != NULL, "history should be allocated");

    const char* history_path = "./isocline_history_dedup_order_metadata.log";
    FILE* file = fopen(history_path, "w");
    EXPECT_TRUE(file != NULL, "history fixture should open");
    for (int i = 0; i < 600; i++) {
        int written = fprintf(file, "# code=%d frequency=%d timestamp=%d\ncommand_%03d\n", i, i + 1,
                              1000 + i, i % 97);
        EXPECT_TRUE(written >= 0, "history fixture should write");
    }
    EXPECT_TRUE(fclose(file) == 0, "history fixture should close");

    (void)history_enable_duplicates(history, true);
    history_load_from(history, history_path, 512);
    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, false), "duplicate snapshot should load");
    EXPECT_TRUE(history_snapshot_count(&snap) == 512,
                "the history limit should apply while duplicates are allowed");
    history_snapshot_free(history, &snap);

    (void)history_enable_duplicates(history, false);
    EXPECT_TRUE(history_snapshot_load(history, &snap, true), "deduplicated snapshot should load");
    EXPECT_TRUE(history_snapshot_count(&snap) == 97, "one entry per command should survive");
    for (int i = 0; i < 97; i++) {
        const int original_index = 599 - i;
        char command[32], code[32], frequency[32], timestamp[32];
        (void)snprintf(command, sizeof(command), "command_%03d", original_index % 97);
        (void)snprintf(code, sizeof(code), "%d", original_index);
        (void)snprintf(frequency, sizeof(frequency), "%d", original_index + 1);
        (void)snprintf(timestamp, sizeof(timestamp), "%d", 1000 + original_index);
        const history_entry_t* entry = history_snapshot_get(&snap, i);
        EXPECT_TRUE(entry != NULL, "surviving history entry should exist");
        EXPECT_STREQ(entry->command, command, "deduplication should preserve recency order");
        EXPECT_STREQ(history_entry_get_metadata(entry, "code"), code,
                     "deduplication should preserve the newest exit code");
        EXPECT_STREQ(history_entry_get_metadata(entry, "frequency"), frequency,
                     "deduplication should preserve the newest frequency");
        EXPECT_STREQ(history_entry_get_metadata(entry, "timestamp"), timestamp,
                     "deduplication should preserve the newest timestamp");
    }
    history_snapshot_free(history, &snap);
    EXPECT_TRUE(history_push(history, "after compaction"),
                "history should remain writable after deduplication");
    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_directory_scope(void) {
    ic_env_t* env = ensure_env();
    EXPECT_TRUE(env != NULL, "isocline environment should exist");
    history_t* original = env->history;
    history_t* history = history_new(test_allocator());
    EXPECT_TRUE(history != NULL, "history should be allocated");
    env->history = history;
    const char* path = "./isocline_history_directory.log";
    history_load_from(history, path, 32);
    history_clear(history);

    const ic_history_metadata_t parent[] = {{"cwd", "/project space/%work"}, {"frequency", "0"}};
    const ic_history_metadata_t child[] = {{"cwd", "/project space/%work/src/deep"},
                                           {"frequency", "0"}};
    const ic_history_metadata_t sibling[] = {{"cwd", "/project space/%work-other"}};
    const ic_history_metadata_t root[] = {{"cwd", "/"}};
    EXPECT_TRUE(history_push(history, "legacy"), "legacy entry should persist");
    EXPECT_TRUE(history_push_with_metadata(history, "shared", parent, 2), "parent should persist");
    EXPECT_TRUE(history_push_with_metadata(history, "shared", child, 2), "child should persist");
    EXPECT_TRUE(history_push_with_metadata(history, "shared", parent, 2), "parent should update");
    EXPECT_TRUE(history_push_with_metadata(history, "sibling", sibling, 1),
                "sibling should persist");
    EXPECT_TRUE(history_push_with_metadata(history, "root", root, 1), "root should persist");
    EXPECT_FALSE(ic_history_directory_is_enabled(), "directory scope should default off");
    EXPECT_FALSE(ic_history_directory_subdirs_is_enabled(), "nested scope should default off");
    EXPECT_TRUE(history_count(history) == 5, "same commands in distinct directories must survive");

    EXPECT_TRUE(ic_set_history_directory("/project space/%work"), "directory should be set");
    EXPECT_FALSE(ic_enable_history_directory(true), "enabling should return the previous state");
    EXPECT_TRUE(history_count(history) == 1,
                "exact scope should exclude child, sibling and legacy");
    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, true), "scoped snapshot should load");
    const history_entry_t* entry = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(entry != NULL, "parent command should remain");
    EXPECT_STREQ(entry->command, "shared", "parent command should match");
    EXPECT_STREQ(history_entry_get_metadata(entry, "frequency"), "2", "frequency is per directory");
    EXPECT_STREQ(history_entry_get_metadata(entry, "cwd"), "/project space/%work",
                 "spaces and percent signs should round trip");
    EXPECT_TRUE(history_snapshot_is_current(history, &snap), "unchanged scope should be current");
    (void)ic_enable_history_directory_subdirs(true);
    EXPECT_FALSE(history_snapshot_is_current(history, &snap), "nested toggle invalidates snapshot");
    EXPECT_TRUE(history_count(history) == 2,
                "nested scope includes descendants, excludes siblings");
    history_snapshot_free(history, &snap);

    history_match_t matches[8];
    ssize_t count = 0;
    EXPECT_TRUE(history_fuzzy_search(history, "shared", matches, 8, &count, NULL),
                "fuzzy search should use directory scope");
    EXPECT_TRUE(count == 2, "both directories should match");
    EXPECT_FALSE(history_fuzzy_search(history, "legacy", matches, 8, &count, NULL),
                 "legacy entries should not leak into scoped search");
    ssize_t idx = -1;
    EXPECT_FALSE(history_search_prefix(history, 0, "sibling", true, &idx),
                 "prefix recall should exclude sibling commands");
    EXPECT_FALSE(history_search(history, 0, "root", true, &idx, NULL),
                 "substring recall should exclude ancestors");

    (void)ic_set_history_directory("/project space/%work/src/deep");
    EXPECT_TRUE(history_count(history) == 1, "child scope must not include parent commands");
    EXPECT_TRUE(history_snapshot_load(history, &snap, true), "child snapshot should load");
    entry = history_snapshot_get(&snap, 0);
    EXPECT_STREQ(history_entry_get_metadata(entry, "frequency"), "1",
                 "child frequency is separate");
    (void)ic_set_history_directory("/");
    EXPECT_FALSE(history_snapshot_is_current(history, &snap),
                 "directory changes invalidate snapshot");
    history_snapshot_free(history, &snap);
    EXPECT_TRUE(history_count(history) == 4,
                "recursive root includes every known absolute directory");
    (void)ic_enable_history_directory_subdirs(false);
    EXPECT_TRUE(history_count(history) == 1, "exact root excludes descendants");
    (void)ic_set_history_directory("/project space/%work/");
    EXPECT_TRUE(history_count(history) == 1, "scope tolerates a trailing slash");
    (void)ic_set_history_directory(NULL);
    EXPECT_TRUE(history_count(history) == 0,
                "unknown current directory must not reuse stale scope");
    history_begin_edit(history);
    EXPECT_TRUE(history_update(history, "unfinished input"), "scratch input should update");
    EXPECT_TRUE(history_count(history) == 1, "scratch input remains available in empty scope");
    (void)history_enable_auto_add(history, false);
    history_end_edit(history, NULL);
    EXPECT_TRUE(history_count(history) == 0, "scratch input must not persist");

    EXPECT_TRUE(ic_enable_history_directory(false), "disabling returns previous state");
    EXPECT_TRUE(history_count(history) == 5, "disabling restores global and legacy history");
    history_free(history);
    history = history_new(test_allocator());
    env->history = history;
    history_load_from(history, path, 32);
    EXPECT_TRUE(history_count(history) == 5, "all directories should survive a new session");
    (void)ic_set_history_directory("/project space/%work");
    (void)ic_enable_history_directory(true);
    EXPECT_TRUE(history_count(history) == 1, "scope should also work after reloading");
    history_clear(history);
    env->history = original;
    history_free(history);
    (void)remove(path);
    return true;
}

static bool test_history_fuzzy_case_toggle(void) {
    ic_env_t* env = ensure_env();
    alloc_t* mem = test_allocator();
    if (env == NULL || mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_case_toggle.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "ls"), "initial lowercase history entry should persist");
    EXPECT_TRUE(history_push(history, "printf hi"), "second entry should persist for contrast");
    EXPECT_TRUE(history_push(history, "MAX"),
                "uppercase history entry should persist for symmetry tests");

    history_match_t matches[4];
    ssize_t match_count = 0;
    bool metadata_filter_applied = false;

    (void)history_set_fuzzy_case_sensitive(history, true);
    EXPECT_FALSE(
        history_fuzzy_search(history, "LS", matches, 4, &match_count, &metadata_filter_applied),
        "case-sensitive search should not match entries with different casing");
    EXPECT_TRUE(match_count == 0, "case-sensitive mismatch should produce zero matches");

    (void)history_set_fuzzy_case_sensitive(history, false);
    EXPECT_TRUE(
        history_fuzzy_search(history, "LS", matches, 4, &match_count, &metadata_filter_applied),
        "case-insensitive search should find matching entries regardless of case");
    EXPECT_TRUE(match_count > 0, "case-insensitive mode should yield results");

    ssize_t reverse_match_count = 0;
    EXPECT_TRUE(
        history_fuzzy_search(history, "max", matches, 4, &reverse_match_count, NULL),
        "case-insensitive search should allow lowercase queries to match uppercase entries");
    EXPECT_TRUE(
        reverse_match_count > 0,
        "lowercase query should match uppercase history entries when case sensitivity is disabled");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_fuzzy_case_toggle_via_api(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    history_t* original = env->history;
    history_t* temp_history = history_new(env->mem);
    if (temp_history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_case_toggle_env.log";
    (void)remove(history_path);
    history_load_from(temp_history, history_path, 16);
    history_clear(temp_history);

    env->history = temp_history;

    EXPECT_TRUE(history_push(temp_history, "ls"),
                "temporary env history should accept initial lowercase entry");
    EXPECT_TRUE(history_push(temp_history, "printf hi"),
                "temporary env history should accept secondary entry");
    EXPECT_TRUE(history_push(temp_history, "MAX"),
                "temporary env history should accept uppercase entry for symmetry tests");

    history_match_t matches[4];
    ssize_t match_count = 0;

    (void)ic_enable_history_fuzzy_case_sensitive(true);
    EXPECT_FALSE(history_fuzzy_search(temp_history, "LS", matches, 4, &match_count, NULL),
                 "case-sensitive env history should not match different casing");
    EXPECT_TRUE(match_count == 0, "case-sensitive env history should produce zero matches");

    (void)ic_enable_history_fuzzy_case_sensitive(false);
    EXPECT_TRUE(history_fuzzy_search(temp_history, "LS", matches, 4, &match_count, NULL),
                "case-insensitive env history should match irrespective of casing");
    EXPECT_TRUE(match_count > 0, "case-insensitive env history should yield matches");

    ssize_t reverse_match_count = 0;
    EXPECT_TRUE(
        history_fuzzy_search(temp_history, "max", matches, 4, &reverse_match_count, NULL),
        "case-insensitive env history should allow lowercase queries to match uppercase entries");
    EXPECT_TRUE(reverse_match_count > 0,
                "lowercase query should match uppercase entries when toggled globally");

    history_clear(temp_history);
    env->history = original;
    history_free(temp_history);
    (void)remove(history_path);
    (void)ic_enable_history_fuzzy_case_sensitive(true);
    return true;
}

static bool test_history_search_sort_api(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    const char* metadata_key = "unexpected";

    EXPECT_TRUE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_RECENT, NULL),
                "history search sort should accept the default recency mode");
    EXPECT_TRUE(ic_get_history_search_sort(&metadata_key) == IC_HISTORY_SEARCH_SORT_RECENT,
                "history search sort getter should report recency mode");
    EXPECT_TRUE(metadata_key == NULL, "recency sort should not expose a metadata key");

    EXPECT_TRUE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_COMMAND_ASC, "ignored"),
                "command sort should ignore an unnecessary metadata key");
    metadata_key = "unexpected";
    EXPECT_TRUE(ic_get_history_search_sort(&metadata_key) == IC_HISTORY_SEARCH_SORT_COMMAND_ASC,
                "history search sort getter should report command ascending mode");
    EXPECT_TRUE(metadata_key == NULL, "command sort should not expose a metadata key");

    EXPECT_TRUE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_METADATA_DESC, "project"),
                "metadata sort should accept a custom metadata key");
    metadata_key = NULL;
    EXPECT_TRUE(ic_get_history_search_sort(&metadata_key) == IC_HISTORY_SEARCH_SORT_METADATA_DESC,
                "history search sort getter should report metadata descending mode");
    EXPECT_STREQ(metadata_key, "project", "metadata sort getter should expose the copied key");

    EXPECT_FALSE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_METADATA_ASC, NULL),
                 "metadata sort should reject a missing metadata key");
    metadata_key = NULL;
    EXPECT_TRUE(ic_get_history_search_sort(&metadata_key) == IC_HISTORY_SEARCH_SORT_METADATA_DESC,
                "failed metadata sort update should preserve the previous mode");
    EXPECT_STREQ(metadata_key, "project", "failed metadata sort update should preserve the key");

    EXPECT_FALSE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_METADATA_ASC, "bad key"),
                 "metadata sort should reject whitespace in metadata keys");
    EXPECT_TRUE(ic_set_history_search_sort(IC_HISTORY_SEARCH_SORT_RECENT, NULL),
                "history search sort should be reset for later tests");
    return true;
}

static bool test_line_wrapping_calculations(void) {
    stringbuf_t* sb = new_stringbuf();
    if (sb == NULL) {
        return false;
    }

    sbuf_replace(sb, "abcd");
    rowcol_t rc = {0};
    (void)sbuf_get_rc_at_pos(sb, 2, 0, 0, true, 3, &rc);
    EXPECT_TRUE(rc.row >= 1, "wrapped rows should advance after terminal width");
    EXPECT_TRUE(rc.col >= 0 && rc.col < 2, "column should stay within terminal width bounds");
    ssize_t roundtrip = sbuf_get_pos_at_rc(sb, 2, 0, 0, true, rc.row, rc.col);
    EXPECT_TRUE(roundtrip == 3, "row/column lookup should round-trip to position");

    sbuf_replace(sb, "line1\nline2");
    rowcol_t multiline = {0};
    (void)sbuf_get_rc_at_pos(sb, 10, 0, 0, true, 6, &multiline);
    EXPECT_TRUE(multiline.row > 0, "newline should advance to next logical row");

    sbuf_replace(sb, "abcdefghij");
    rowcol_t wide = {0};
    (void)sbuf_get_rc_at_pos(sb, 10, 0, 0, true, 7, &wide);
    rowcol_t shrink = {0};
    (void)sbuf_get_wrapped_rc_at_pos(sb, 10, 5, 0, 0, true, 7, &shrink);
    EXPECT_TRUE(shrink.row >= wide.row, "shrinking the terminal should not decrease row index");
    EXPECT_TRUE(shrink.col >= 0 && shrink.col < 5,
                "shrinking the terminal should recompute wrapped columns");

    const struct {
        const char* input;
        ssize_t rows;
        ssize_t row;
        ssize_t col;
    } full_width_cases[] = {
        {"1234567", 1, 0, 7},    {"12345678", 2, 1, 0},    {"123456789", 2, 1, 1},
        {"12345678\n", 2, 1, 0}, {"12345678\nx", 2, 1, 1}, {"12345678abcdefg", 3, 2, 0},
        {"123456界", 2, 1, 0},   {"1234567界", 2, 1, 2},   {"1234567e\xCC\x81", 2, 1, 0},
    };
    for (size_t i = 0; i < sizeof(full_width_cases) / sizeof(full_width_cases[0]); i++) {
        sbuf_replace(sb, full_width_cases[i].input);
        ssize_t rows = sbuf_get_rc_at_pos(sb, 10, 2, 3, false, sbuf_len(sb), &rc);
        EXPECT_TRUE(rows == full_width_cases[i].rows,
                    "hidden marker should use the full width without extra newline rows");
        EXPECT_TRUE(
            rc.row == full_width_cases[i].row && rc.col == full_width_cases[i].col,
            "cursor should follow the full-width input, including wide/combining characters");
        for (ssize_t pos = 0; pos >= 0 && pos <= sbuf_len(sb); pos = sbuf_next(sb, pos, NULL)) {
            (void)sbuf_get_rc_at_pos(sb, 10, 2, 3, false, pos, &rc);
            EXPECT_TRUE(sbuf_get_pos_at_rc(sb, 10, 2, 3, false, rc.row, rc.col) == pos,
                        "full-width row/column lookup should round-trip character positions");
        }
    }

    sbuf_replace(sb, "12345678x");
    ssize_t resized_rows = sbuf_get_wrapped_rc_at_pos(sb, 10, 8, 2, 3, false, sbuf_len(sb), &rc);
    EXPECT_TRUE(resized_rows == 3 && rc.row == 2 && rc.col == 4,
                "resize should count full-width rows without adding a hidden marker");

    const struct {
        const char* input;
        ssize_t rows;
        ssize_t row;
        ssize_t col;
    } marker_cases[] = {
        {"1234567", 1, 0, 7},       {"12345678", 2, 1, 1},        {"1234567\nx", 2, 1, 1},
        {"1234567abcdef", 2, 1, 6}, {"1234567abcdefg", 3, 2, 1},  {"12345界", 1, 0, 7},
        {"123456界", 2, 1, 2},      {"123456e\xCC\x81", 1, 0, 7},
    };
    for (size_t i = 0; i < sizeof(marker_cases) / sizeof(marker_cases[0]); i++) {
        sbuf_replace(sb, marker_cases[i].input);
        ssize_t rows = sbuf_get_rc_at_pos(sb, 10, 2, 3, true, sbuf_len(sb), &rc);
        EXPECT_TRUE(rows == marker_cases[i].rows,
                    "visible marker should reserve only its single column");
        EXPECT_TRUE(rc.row == marker_cases[i].row && rc.col == marker_cases[i].col,
                    "cursor should use the column immediately before a visible wrap marker");
    }

    sbuf_replace(sb, "1234567x");
    resized_rows = sbuf_get_wrapped_rc_at_pos(sb, 10, 10, 2, 3, true, sbuf_len(sb), &rc);
    EXPECT_TRUE(resized_rows == 2 && rc.row == 1 && rc.col == 4,
                "a marker in the final column should not count as a hard wrap");

    sbuf_replace(sb, "123456x");
    ssize_t rows = sbuf_get_rc_at_pos(sb, 10, 2, 3, 2, sbuf_len(sb), &rc);
    EXPECT_TRUE(rows == 2 && rc.row == 1 && rc.col == 1,
                "a wide marker should reserve two columns before wrapping");
    for (ssize_t pos = 0; pos >= 0 && pos <= sbuf_len(sb); pos = sbuf_next(sb, pos, NULL)) {
        (void)sbuf_get_rc_at_pos(sb, 10, 2, 3, 2, pos, &rc);
        EXPECT_TRUE(sbuf_get_pos_at_rc(sb, 10, 2, 3, 2, rc.row, rc.col) == pos,
                    "wide marker row/column lookup should round-trip character positions");
    }
    resized_rows = sbuf_get_wrapped_rc_at_pos(sb, 10, 9, 2, 3, 2, sbuf_len(sb), &rc);
    EXPECT_TRUE(resized_rows == 3 && rc.row == 2 && rc.col == 4,
                "resize should account for a wide marker that no longer fits its old row");

    sbuf_free(sb);
    return true;
}

static bool test_unicode_decode_utf8_valid_sequences(void) {
    typedef struct utf8_case_s {
        const uint8_t* data;
        ssize_t len;
        unicode_codepoint_t expected_codepoint;
        ssize_t expected_bytes;
    } utf8_case_t;

    static const uint8_t one_byte[] = {0x24};                     // U+0024 '$'
    static const uint8_t two_byte[] = {0xC2, 0xA2};               // U+00A2 '¢'
    static const uint8_t three_byte[] = {0xE2, 0x82, 0xAC};       // U+20AC '€'
    static const uint8_t four_byte[] = {0xF0, 0x9F, 0x98, 0x80};  // U+1F600 '😀'

    const utf8_case_t cases[] = {
        {one_byte, (ssize_t)sizeof(one_byte), 0x24, 1},
        {two_byte, (ssize_t)sizeof(two_byte), 0x00A2, 2},
        {three_byte, (ssize_t)sizeof(three_byte), 0x20AC, 3},
        {four_byte, (ssize_t)sizeof(four_byte), 0x1F600, 4},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unicode_codepoint_t codepoint = 0;
        ssize_t bytes_read = 0;
        bool ok = unicode_decode_utf8(cases[i].data, cases[i].len, &codepoint, &bytes_read);
        EXPECT_TRUE(ok, "unicode_decode_utf8 should accept valid UTF-8 sequences");
        EXPECT_TRUE(codepoint == cases[i].expected_codepoint,
                    "decoded codepoint should match expected value");
        EXPECT_TRUE(bytes_read == cases[i].expected_bytes,
                    "decoded byte count should match expected sequence length");
    }

    return true;
}

static bool test_unicode_decode_utf8_invalid_sequences(void) {
    typedef struct utf8_invalid_case_s {
        const uint8_t* data;
        ssize_t len;
        uint8_t expected_fallback;
    } utf8_invalid_case_t;

    static const uint8_t lone_continuation[] = {0x80};
    static const uint8_t overlong_two[] = {0xC0, 0xAF};
    static const uint8_t overlong_three[] = {0xE0, 0x80, 0x80};
    static const uint8_t overlong_four[] = {0xF0, 0x80, 0x80, 0x80};
    static const uint8_t surrogate_half[] = {0xED, 0xA0, 0x80};
    static const uint8_t too_large[] = {0xF4, 0x90, 0x80, 0x80};
    static const uint8_t bad_continuation[] = {0xE2, 0x28, 0xA1};
    static const uint8_t truncated_three[] = {0xE2};

    const utf8_invalid_case_t cases[] = {
        {lone_continuation, (ssize_t)sizeof(lone_continuation), 0x80},
        {overlong_two, (ssize_t)sizeof(overlong_two), 0xC0},
        {overlong_three, (ssize_t)sizeof(overlong_three), 0xE0},
        {overlong_four, (ssize_t)sizeof(overlong_four), 0xF0},
        {surrogate_half, (ssize_t)sizeof(surrogate_half), 0xED},
        {too_large, (ssize_t)sizeof(too_large), 0xF4},
        {bad_continuation, (ssize_t)sizeof(bad_continuation), 0xE2},
        {truncated_three, (ssize_t)sizeof(truncated_three), 0xE2},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unicode_codepoint_t codepoint = 0;
        ssize_t bytes_read = 0;
        bool ok = unicode_decode_utf8(cases[i].data, cases[i].len, &codepoint, &bytes_read);
        EXPECT_FALSE(ok, "unicode_decode_utf8 should reject invalid UTF-8 sequences");
        EXPECT_TRUE(codepoint == (unicode_codepoint_t)cases[i].expected_fallback,
                    "invalid UTF-8 should fall back to the first byte value");
        EXPECT_TRUE(bytes_read == 1,
                    "invalid UTF-8 should consume exactly one byte for forward progress");
    }

    unicode_codepoint_t codepoint = 1234;
    ssize_t bytes_read = 99;
    EXPECT_FALSE(unicode_decode_utf8(NULL, 1, &codepoint, &bytes_read),
                 "decode should fail for NULL input");
    EXPECT_TRUE(codepoint == 0, "NULL input should reset decoded codepoint to zero");
    EXPECT_TRUE(bytes_read == 0, "NULL input should report zero bytes consumed");

    codepoint = 1234;
    bytes_read = 99;
    EXPECT_FALSE(unicode_decode_utf8((const uint8_t*)"A", 0, &codepoint, &bytes_read),
                 "decode should fail for zero-length input");
    EXPECT_TRUE(codepoint == 0, "zero-length input should reset decoded codepoint to zero");
    EXPECT_TRUE(bytes_read == 0, "zero-length input should report zero bytes consumed");

    return true;
}

static bool test_unicode_encode_utf8_roundtrip(void) {
    const unicode_codepoint_t cps[] = {0x24, 0x00A2, 0x20AC, 0x1F600};

    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); ++i) {
        uint8_t encoded[4] = {0, 0, 0, 0};
        int encoded_len = unicode_encode_utf8(cps[i], encoded);
        EXPECT_TRUE(encoded_len > 0 && encoded_len <= 4,
                    "unicode_encode_utf8 should encode valid codepoints");

        unicode_codepoint_t decoded = 0;
        ssize_t bytes_read = 0;
        EXPECT_TRUE(unicode_decode_utf8(encoded, encoded_len, &decoded, &bytes_read),
                    "encoded bytes should decode successfully");
        EXPECT_TRUE(decoded == cps[i], "decoded codepoint should match original encoded value");
        EXPECT_TRUE(bytes_read == encoded_len,
                    "decoder byte count should match encoded byte length");
    }

    uint8_t out[4] = {0, 0, 0, 0};
    EXPECT_TRUE(unicode_encode_utf8(0x110000, out) == 0,
                "encoder should reject codepoints above Unicode maximum");
    EXPECT_TRUE(unicode_encode_utf8(0xD800, out) == 0,
                "encoder should reject UTF-16 surrogate codepoints");
    return true;
}

static bool test_unicode_qutf8_raw_byte_roundtrip(void) {
    const uint8_t invalid_utf8[] = {0xFF};
    ssize_t read = 0;
    unicode_t u = unicode_from_qutf8(invalid_utf8, (ssize_t)sizeof(invalid_utf8), &read);

    uint8_t recovered_raw = 0;
    EXPECT_TRUE(read == 1, "qutf8 decoder should consume one byte for invalid UTF-8");
    EXPECT_TRUE(unicode_is_raw(u, &recovered_raw),
                "invalid UTF-8 should map to a raw-plane Unicode sentinel");
    EXPECT_TRUE(recovered_raw == 0xFF,
                "raw-plane sentinel should preserve original invalid byte value");

    uint8_t qutf8_out[5] = {0, 0, 0, 0, 0};
    unicode_to_qutf8(u, qutf8_out);
    EXPECT_TRUE(qutf8_out[0] == 0xFF && qutf8_out[1] == 0,
                "raw-plane Unicode should encode back into the original raw byte");

    static const uint8_t euro[] = {0xE2, 0x82, 0xAC};
    read = 0;
    unicode_t euro_u = unicode_from_qutf8(euro, (ssize_t)sizeof(euro), &read);
    EXPECT_TRUE(read == 3, "valid UTF-8 should decode to Unicode in qutf8 decoder");
    EXPECT_TRUE(euro_u == 0x20AC, "valid UTF-8 should decode to expected codepoint");

    memset(qutf8_out, 0, sizeof(qutf8_out));
    unicode_to_qutf8(euro_u, qutf8_out);
    EXPECT_TRUE(qutf8_out[0] == 0xE2 && qutf8_out[1] == 0x82 && qutf8_out[2] == 0xAC,
                "Unicode codepoint should encode back to canonical UTF-8 bytes");

    return true;
}

static bool test_unicode_width_calculation_with_invalid_and_ansi(void) {
    static const uint8_t mixed[] = {'A', 0xE2, 0x82, 0xAC, 0x80, 'B'};
    size_t width = unicode_calculate_utf8_width((const char*)mixed, sizeof(mixed));
    EXPECT_TRUE(width == 4,
                "utf8 width should count ASCII, valid UTF-8, and invalid bytes deterministically");

    static const uint8_t ansi_and_invalid[] = {'\x1B', '[', '3', '1', 'm', 'A',
                                               '\x1B', '[', '0', 'm', 0x80};
    size_t ansi_chars = 0;
    size_t visible_chars = 0;
    size_t display_width = unicode_calculate_display_width(
        (const char*)ansi_and_invalid, sizeof(ansi_and_invalid), &ansi_chars, &visible_chars);
    EXPECT_TRUE(display_width == 2,
                "display width should ignore ANSI escapes while counting invalid byte fallback");
    EXPECT_TRUE(ansi_chars == 9,
                "ANSI byte counter should include bytes belonging to both escape sequences");
    EXPECT_TRUE(visible_chars == 2,
                "visible character count should include regular glyphs and invalid byte fallback");

    return true;
}

static bool test_str_next_ofs_with_utf8_and_escape_sequences(void) {
    static const char sample[] = {'A', (char)0xE2, (char)0x82, (char)0xAC, '\x1B', '[',
                                  '3', '1',        'm',        'B',        0};
    ssize_t width = 0;

    ssize_t ofs_a = str_next_ofs(sample, 10, 0, &width);
    EXPECT_TRUE(ofs_a == 1, "ASCII codepoint should consume one byte");
    EXPECT_TRUE(width == 1, "ASCII width should be one column");

    ssize_t ofs_euro = str_next_ofs(sample, 10, 1, &width);
    EXPECT_TRUE(ofs_euro == 3, "UTF-8 multibyte sequence should consume all continuation bytes");
    EXPECT_TRUE(width == 1, "Euro sign should render as single column");

    ssize_t ofs_esc = str_next_ofs(sample, 10, 4, &width);
    EXPECT_TRUE(ofs_esc == 5, "CSI escape should be treated as one logical sequence");

    ssize_t ofs_b = str_next_ofs(sample, 10, 9, &width);
    EXPECT_TRUE(ofs_b == 1, "trailing ASCII character should consume one byte");

    ssize_t ofs_end = str_next_ofs(sample, 10, 10, &width);
    EXPECT_TRUE(ofs_end == 0, "str_next_ofs should return zero at end of buffer");

    static const char continuation_cluster[] = {'x', (char)0x80, (char)0x81, 'y', 0};
    ssize_t ofs_cluster = str_next_ofs(continuation_cluster, 4, 1, NULL);
    EXPECT_TRUE(ofs_cluster == 2,
                "invalid leading byte should still advance through contiguous continuation bytes");

    return true;
}

static bool test_stringbuf_empty_non_utf8_result(void) {
    stringbuf_t* sb = new_stringbuf();
    EXPECT_TRUE(sb != NULL, "empty input buffer should allocate");

    char* result = sbuf_strdup_from_utf8(sb);
    EXPECT_STREQ(result, "", "empty non-UTF-8 input should be a successful empty submission");
    ic_free(result);

    sbuf_append(sb, "request");
    sbuf_clear(sb);
    result = sbuf_strdup_from_utf8(sb);
    EXPECT_STREQ(result, "", "clearing an agent request should preserve empty submission");
    ic_free(result);
    sbuf_free(sb);
    return true;
}

static bool test_stringbuf_utf8_navigation_and_deletion(void) {
    stringbuf_t* sb = new_stringbuf();
    if (sb == NULL) {
        return false;
    }

    sbuf_replace(sb,
                 "a\xE2\x82\xAC"
                 "b");
    EXPECT_TRUE(sbuf_len(sb) == 5, "buffer should contain one ASCII, one UTF-8 glyph, one ASCII");

    ssize_t width = 0;
    ssize_t pos = sbuf_next(sb, 0, &width);
    EXPECT_TRUE(pos == 1 && width == 1, "moving over ASCII should advance by one byte");

    pos = sbuf_next(sb, pos, &width);
    EXPECT_TRUE(pos == 4 && width == 1,
                "moving over UTF-8 glyph should advance by full codepoint byte length");

    pos = sbuf_next(sb, pos, &width);
    EXPECT_TRUE(pos == 5 && width == 1, "moving over final ASCII byte should reach buffer end");

    pos = sbuf_prev(sb, pos, &width);
    EXPECT_TRUE(pos == 4 && width == 1, "reverse movement should step back over ASCII byte");

    pos = sbuf_prev(sb, pos, &width);
    EXPECT_TRUE(pos == 1 && width == 1,
                "reverse movement should step back over entire UTF-8 glyph atomically");

    ssize_t new_pos = sbuf_delete_char_before(sb, 4);
    EXPECT_TRUE(new_pos == 1,
                "deleting before UTF-8 boundary should land at start of deleted glyph");
    EXPECT_STREQ(sbuf_string(sb), "ab",
                 "deleting UTF-8 glyph should remove all bytes of codepoint");

    sbuf_free(sb);
    return true;
}

static bool test_push_raw_input_preconditions(void) {
    static const uint8_t bytes[] = {'a', 'b', 'c'};
    EXPECT_TRUE(ic_push_raw_input(bytes, 0), "zero-length raw input should be accepted as no-op");

    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    tty_t* saved_tty = env->tty;
    env->tty = NULL;
    EXPECT_FALSE(ic_push_raw_input(bytes, sizeof(bytes)),
                 "raw input push should fail when no TTY backend is available");
    EXPECT_FALSE(ic_push_key_event(KEY_ENTER),
                 "single key event push should fail when no TTY backend is available");
    EXPECT_FALSE(ic_push_key_sequence((const ic_keycode_t[]){KEY_ENTER, KEY_TAB}, 2),
                 "key sequence push should fail when no TTY backend is available");
    env->tty = saved_tty;

    return true;
}

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
    code_t pushbuf[32];
    ssize_t push_count;
    uint8_t cpushbuf[256];
    ssize_t cpush_count;
    stringbuf_t* typeahead_replay;
    ssize_t typeahead_replay_pos;
    bool typeahead_capture_mode;
    bool typeahead_crlf_swapped;
};

static bool test_tty_character_pushback_capacity_guard(void) {
    struct tty_s tty_probe;
    memset(&tty_probe, 0, sizeof(tty_probe));

    const size_t capacity = sizeof(tty_probe.cpushbuf);
    for (size_t i = 0; i <= capacity; ++i) {
        tty_cpush_char((tty_t*)&tty_probe, (uint8_t)('a' + (i % 26)));
    }

    EXPECT_TRUE(tty_probe.cpush_count == (ssize_t)capacity,
                "character pushback buffer should clamp at its capacity");

    size_t pops = 0;
    uint8_t c = 0;
    while (tty_cpop((tty_t*)&tty_probe, &c)) {
        pops++;
    }
    EXPECT_TRUE(pops == capacity, "character pushback pop count should match clamped capacity");

    return true;
}

static bool test_push_raw_input_null_pointer_rejected(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    struct tty_s tty_probe;
    memset(&tty_probe, 0, sizeof(tty_probe));

    tty_t* saved_tty = env->tty;
    env->tty = (tty_t*)&tty_probe;

    EXPECT_FALSE(ic_push_raw_input(NULL, 3),
                 "non-empty raw input push should reject NULL data buffers");

    env->tty = saved_tty;
    return true;
}

static bool test_tty_capture_pending_raw_preconditions(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    EXPECT_FALSE(tty_capture_pending_raw(NULL, out), "pending raw capture should reject NULL tty");
    EXPECT_TRUE(sbuf_len(out) == 0, "failed capture should leave output buffer empty");

    struct tty_s tty_probe;
    memset(&tty_probe, 0, sizeof(tty_probe));
    tty_probe.fd_in = -1;
    (void)sbuf_append(out, "stale");
    EXPECT_FALSE(tty_capture_pending_raw((tty_t*)&tty_probe, out),
                 "pending raw capture should reject non-tty file descriptors");
    EXPECT_TRUE(sbuf_len(out) == 0, "failed capture should clear stale output");

    sbuf_free(out);
    return true;
}

static bool test_escape_skip_charset_sequence_length(void) {
    ssize_t esc_len = 0;
    EXPECT_TRUE(skip_esc("\x1b(B", 3, &esc_len),
                "shared escape skipper should identify charset-shift sequences");
    EXPECT_TRUE(esc_len == 3, "charset-shift escape sequence should consume all three bytes");

    esc_len = 0;
    EXPECT_TRUE(skip_esc("\x1b%G", 3, &esc_len),
                "shared escape skipper should identify percent charset sequences");
    EXPECT_TRUE(esc_len == 3, "percent charset escape sequence should consume all three bytes");

    return true;
}

static bool test_typeahead_toggle_lifecycle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(false);
    EXPECT_FALSE(ic_typeahead_is_enabled(), "typeahead should be disabled after reset");

    bool previous = ic_enable_typeahead(true);
    EXPECT_FALSE(previous, "enabling typeahead should report previous disabled state");
    EXPECT_TRUE(ic_typeahead_is_enabled(), "typeahead should be enabled");

    previous = ic_enable_typeahead(true);
    EXPECT_TRUE(previous, "enabling typeahead again should report previous enabled state");
    EXPECT_TRUE(ic_typeahead_is_enabled(), "typeahead should remain enabled");

    previous = ic_enable_typeahead(false);
    EXPECT_TRUE(previous, "disabling typeahead should report previous enabled state");
    EXPECT_FALSE(ic_typeahead_is_enabled(), "typeahead should be disabled");
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) == 0,
                "disabling typeahead should clear pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "disabling typeahead should clear pending raw bytes");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_clear_pending_state(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"clear-me", strlen("clear-me")),
                "enabled typeahead should ingest raw bytes");
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) > 0,
                "ingest should create pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) > 0,
                "ingest should retain pending raw bytes until prepare");

    ic_typeahead_clear();
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) == 0,
                "clear should remove pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "clear should remove pending raw bytes");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_capture_gate_registration_and_disabled_state(void) {
    reset_typeahead_test_state(false);
    g_typeahead_capture_gate_calls = 0;
    g_typeahead_capture_gate_result = true;
    g_typeahead_capture_gate_last_arg = NULL;
    ic_set_typeahead_capture_allowed_callback(stub_typeahead_capture_allowed, (void*)0xBEEF);

    EXPECT_FALSE(ic_typeahead_capture_available_input(),
                 "disabled typeahead should not attempt capture");
    EXPECT_TRUE(g_typeahead_capture_gate_calls == 0,
                "disabled typeahead should not call the capture gate");

    (void)ic_enable_typeahead(true);
    g_typeahead_capture_gate_result = false;
    EXPECT_FALSE(ic_typeahead_capture_available_input(),
                 "capture gate returning false should block capture");
    EXPECT_TRUE(g_typeahead_capture_gate_calls == 1,
                "enabled capture should call the capture gate once");
    EXPECT_TRUE(g_typeahead_capture_gate_last_arg == (void*)0xBEEF,
                "capture gate should receive the configured argument");

    g_typeahead_capture_gate_result = true;
    (void)ic_typeahead_capture_available_input();
    EXPECT_TRUE(g_typeahead_capture_gate_calls == 2,
                "allowed capture should still consult the capture gate");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_filter_preserves_plain_text(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char* input = "echo plain\ttext\n";
    ic_typeahead_filter_escape_sequences_into(input, strlen(input), out);
    EXPECT_STREQ(sbuf_string(out), "echo plain\ttext\n",
                 "plain printable text, tabs, and newlines should pass through");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_filter_removes_csi_sequences(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char* input = "ab\x1b[C\x1b[1;5Dcd\x1b[?25hef";
    ic_typeahead_filter_escape_sequences_into(input, strlen(input), out);
    EXPECT_STREQ(sbuf_string(out), "abcdef",
                 "CSI cursor/control sequences should be removed from visible input");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_filter_removes_osc_sequences(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char* bel_input = "pre\x1b]0;title\x07post";
    ic_typeahead_filter_escape_sequences_into(bel_input, strlen(bel_input), out);
    EXPECT_STREQ(sbuf_string(out), "prepost", "OSC BEL-terminated sequence should be removed");

    const char* st_input = "pre\x1b]0;title\x1b\\post";
    ic_typeahead_filter_escape_sequences_into(st_input, strlen(st_input), out);
    EXPECT_STREQ(sbuf_string(out), "prepost", "OSC ST-terminated sequence should be removed");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_filter_removes_charset_numeric_and_control_bytes(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'a', '\x1b', '(',    'B', 'b',    '\x1b', '1', '2',
                          '3', 'c',    '\x07', 'd', '\x03', 'e',    '\0'};
    ic_typeahead_filter_escape_sequences_into(input, strlen(input), out);
    EXPECT_STREQ(sbuf_string(out), "abcde",
                 "charset, numeric escape, bell, and control bytes should be removed");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_filter_reuses_extended_escape_skipper(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char* input =
        "a\x1bPignored\x07"
        "b\x1b_ignored\x1b\\c\x1b%Gd";
    ic_typeahead_filter_escape_sequences_into(input, strlen(input), out);
    EXPECT_STREQ(sbuf_string(out), "abcd",
                 "typeahead filter should use shared escape skipper for DCS/PM/charset escapes");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_backspace_and_delete(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'a', 'b', 'c', '\b', 'd', 0x7F, 'e'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "abe",
                 "backspace and delete should remove the previous normalized character");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_backspace_deletes_full_utf8_codepoint(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'a', (char)0xC3, (char)0xA9, '\b', 'b'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "ab",
                 "backspace should delete the full previous UTF-8 codepoint");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_ctrl_u_keeps_previous_lines(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'o', 'n', 'e', '\n', 't', 'w', 'o', 0x15, 'r', 'e', 'd', 'o'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "one\nredo",
                 "Ctrl-U should delete only back to the current line start");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_ctrl_u_stops_at_carriage_return(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'o', 'n', 'e', '\r', 't', 'w', 'o', 0x15, 'r', 'e', 'd', 'o'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "one\rredo",
                 "Ctrl-U must not erase a command before a typeahead Return");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_ctrl_w_deletes_previous_word(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char* input = "cmd alpha   beta";
    ic_typeahead_normalize_line_edit_sequences_into(input, strlen(input), out);
    EXPECT_STREQ(sbuf_string(out), "cmd alpha   beta",
                 "baseline normalize should preserve text without control bytes");

    const char input_with_ctrl_w[] = {'c', 'm', 'd', ' ', 'a', 'l',  'p', 'h', 'a', ' ', ' ',
                                      ' ', 'b', 'e', 't', 'a', 0x17, 'g', 'a', 'm', 'm', 'a'};
    ic_typeahead_normalize_line_edit_sequences_into(input_with_ctrl_w, sizeof(input_with_ctrl_w),
                                                    out);
    EXPECT_STREQ(sbuf_string(out), "cmd alpha   gamma",
                 "Ctrl-W should remove trailing space and the previous word");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_ctrl_w_deletes_utf8_word(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'c',        'm',        'd',        ' ',  (char)0xC3, (char)0xA9,
                          (char)0xE2, (char)0x82, (char)0xAC, 0x17, 'x'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "cmd x",
                 "Ctrl-W should delete complete UTF-8 codepoints in the previous word");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_normalize_ctrl_w_stops_at_carriage_return(void) {
    stringbuf_t* out = new_stringbuf();
    EXPECT_TRUE(out != NULL, "test string buffer allocation should succeed");

    const char input[] = {'o', 'n', 'e', '\r', 't', 'w', 'o', 0x17, 'x'};
    ic_typeahead_normalize_line_edit_sequences_into(input, sizeof(input), out);
    EXPECT_STREQ(sbuf_string(out), "one\rx",
                 "Ctrl-W must not cross a typeahead Return into the previous command");

    sbuf_free(out);
    return true;
}

static bool test_typeahead_ingest_rejects_disabled_null_and_empty_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(false);
    EXPECT_FALSE(ic_typeahead_ingest_raw_input((const uint8_t*)"abc", 3),
                 "disabled typeahead should reject manual raw ingest");

    (void)ic_enable_typeahead(true);
    EXPECT_FALSE(ic_typeahead_ingest_raw_input(NULL, 3),
                 "manual raw ingest should reject NULL data");
    EXPECT_FALSE(ic_typeahead_ingest_raw_input((const uint8_t*)"abc", 0),
                 "manual raw ingest should reject empty data");
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) == 0,
                "rejected ingest should not create pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "rejected ingest should not create pending raw bytes");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_plain_text_sets_pending_initial_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"echo ok", strlen("echo ok")),
                "plain text ingest should succeed");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "echo ok",
                 "plain text should become pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == (ssize_t)strlen("echo ok"),
                "raw bytes should remain pending until readline preparation");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_appends_to_existing_pending_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"git ", strlen("git ")),
                "first ingest should succeed");
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"status", strlen("status")),
                "second ingest should succeed");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "git status",
                 "successive visible ingests should append to pending initial input");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_filters_escape_sequences_before_pending_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    const char* raw =
        "ab\x1b[C\x1b]0;ignored\x07"
        "cd";
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)raw, strlen(raw)),
                "escape-rich raw input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "abcd",
                 "escape sequences should not appear in pending initial input");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_preserves_carriage_return_as_submit(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"make test\r", strlen("make test\r")),
                "CR-terminated input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "make test\r",
                 "carriage return should remain distinguishable as a full Return");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_preserves_ctrl_j_as_line_feed(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"first command\nsecond",
                                              strlen("first command\nsecond")),
                "multiline pending input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "first command\nsecond",
                 "Ctrl+J line feed should remain in the current multiline input");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_keeps_last_submitted_line_with_trailing_return(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    const char* raw = "first\rsecond\r";
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)raw, strlen(raw)),
                "trailing-Return input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "second\r",
                 "pending initial input should keep the last submitted line");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_returns_only_clear_pending_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"\r\r", strlen("\r\r")),
                "returns-only input should ingest");
    EXPECT_TRUE(ic_typeahead_pending_initial_input(env) == NULL,
                "returns-only pending input should be cleared");
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) == 0,
                "returns-only pending input length should be zero");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_line_feeds_only_remain_editable(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    static const uint8_t raw[] = {'\n', '\n'};
    EXPECT_TRUE(ic_typeahead_ingest_raw_input(raw, sizeof(raw)), "Ctrl+J-only input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "\n\n",
                 "Ctrl+J line feeds must not be mistaken for Return");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_ingest_chunked_ctrl_j_then_return(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"alpha\n", 6),
                "first chunk ending in Ctrl+J should ingest");
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"beta", 4),
                "text following Ctrl+J should append");
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"\r", 1),
                "final Return chunk should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "alpha\nbeta\r",
                 "chunk boundaries must not blur Ctrl+J and Return semantics");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_prepare_clears_raw_without_controls_and_keeps_initial_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    g_typeahead_capture_gate_result = false;
    ic_set_typeahead_capture_allowed_callback(stub_typeahead_capture_allowed, NULL);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"abc", strlen("abc")),
                "plain raw input should ingest");
    ic_typeahead_prepare_for_readline(env);

    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "abc",
                 "prepare should keep visible initial input when no control bytes are present");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "prepare should clear raw bytes that do not need replay");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_prepare_replays_control_raw_bytes_in_original_order(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    g_typeahead_capture_gate_result = false;
    ic_set_typeahead_capture_allowed_callback(stub_typeahead_capture_allowed, NULL);

    static const uint8_t raw[] = {'a', 'b', 0x7F, 'c', 'd'};
    EXPECT_TRUE(ic_typeahead_ingest_raw_input(raw, sizeof(raw)), "control raw input should ingest");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "acd",
                 "normalized pending input should reflect line editing before replay");

    struct tty_s tty_probe;
    memset(&tty_probe, 0, sizeof(tty_probe));
    tty_probe.typeahead_replay = new_stringbuf();
    EXPECT_TRUE(tty_probe.typeahead_replay != NULL,
                "typeahead replay queue allocation should succeed");
    tty_t* saved_tty = env->tty;
    env->tty = (tty_t*)&tty_probe;

    ic_typeahead_prepare_for_readline(env);
    EXPECT_TRUE(ic_typeahead_pending_initial_input(env) == NULL,
                "successful raw control replay should clear pending initial input");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "successful raw control replay should clear pending raw bytes");
    EXPECT_TRUE(tty_typeahead_replay_count((tty_t*)&tty_probe) == (ssize_t)sizeof(raw),
                "raw replay should queue every captured byte without using parser pushback");
    EXPECT_TRUE(memcmp(sbuf_string(tty_probe.typeahead_replay), raw, sizeof(raw)) == 0,
                "raw replay queue should preserve captured byte order");

    env->tty = saved_tty;
    sbuf_free(tty_probe.typeahead_replay);
    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_prepare_failed_control_replay_preserves_initial_input(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    g_typeahead_capture_gate_result = false;
    ic_set_typeahead_capture_allowed_callback(stub_typeahead_capture_allowed, NULL);

    static const uint8_t raw[] = {'r', 'u', 'n', '\r'};
    EXPECT_TRUE(ic_typeahead_ingest_raw_input(raw, sizeof(raw)), "Return raw input should ingest");

    tty_t* saved_tty = env->tty;
    env->tty = NULL;
    ic_typeahead_prepare_for_readline(env);
    env->tty = saved_tty;

    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "run\r",
                 "failed raw replay should preserve normalized initial input as fallback");
    EXPECT_TRUE(ic_typeahead_pending_raw_byte_count(env) == 0,
                "failed raw replay should still clear pending raw bytes");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_typeahead_pending_input_hidden_when_disabled(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    reset_typeahead_test_state(true);
    EXPECT_TRUE(ic_typeahead_ingest_raw_input((const uint8_t*)"hidden", strlen("hidden")),
                "pending input should ingest before disabling");
    EXPECT_STREQ(ic_typeahead_pending_initial_input(env), "hidden",
                 "pending input should be visible while typeahead is enabled");

    (void)ic_enable_typeahead(false);
    EXPECT_TRUE(ic_typeahead_pending_initial_input(env) == NULL,
                "pending input should not be visible after disabling typeahead");
    EXPECT_TRUE(ic_typeahead_pending_initial_input_len(env) == 0,
                "disabling typeahead should clear pending input length");

    reset_typeahead_test_state(false);
    return true;
}

static bool test_prompt_marker_roundtrip(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_set_prompt_marker("=> ", ".. ");
    EXPECT_STREQ(ic_get_prompt_marker(), "=> ", "primary prompt marker should round-trip");
    EXPECT_STREQ(ic_get_continuation_prompt_marker(), ".. ",
                 "continuation prompt marker should round-trip");

    ic_set_prompt_marker(":: ", NULL);
    EXPECT_STREQ(ic_get_prompt_marker(), ":: ", "primary marker should update to custom value");
    EXPECT_STREQ(ic_get_continuation_prompt_marker(),
                 ":: ", "NULL continuation marker should mirror primary marker");

    ic_set_prompt_marker(NULL, NULL);
    EXPECT_STREQ(ic_get_prompt_marker(), "> ", "NULL primary marker should restore default marker");
    EXPECT_STREQ(ic_get_continuation_prompt_marker(), "> ",
                 "NULL continuation marker should restore default continuation marker");

    return true;
}

static bool test_menu_prompt_api_roundtrip(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_set_history_search_prompt(NULL);
    ic_set_command_palette_prompt(NULL);
    EXPECT_STREQ(ic_get_history_search_prompt(),
                 "history search: ", "history search prompt default should be restored by NULL");
    EXPECT_STREQ(ic_get_command_palette_prompt(),
                 "command palette: ", "command palette prompt default should be restored by NULL");

    char history_prompt[] = "hist> ";
    char palette_prompt[] = "cmd> ";
    ic_set_history_search_prompt(history_prompt);
    ic_set_command_palette_prompt(palette_prompt);
    history_prompt[0] = 'X';
    palette_prompt[0] = 'X';

    EXPECT_STREQ(ic_get_history_search_prompt(), "hist> ",
                 "history search prompt setter should copy caller storage");
    EXPECT_STREQ(ic_get_command_palette_prompt(), "cmd> ",
                 "command palette prompt setter should copy caller storage");

    ic_set_history_search_prompt(NULL);
    ic_set_command_palette_prompt(NULL);
    EXPECT_STREQ(ic_get_history_search_prompt(),
                 "history search: ", "history search prompt should reset to default");
    EXPECT_STREQ(ic_get_command_palette_prompt(),
                 "command palette: ", "command palette prompt should reset to default");

    return true;
}

static bool test_hint_delay_clamps(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->hint_delay = 250;
    long previous = ic_set_hint_delay(-1);
    EXPECT_TRUE(previous == 250, "hint delay setter should return previous value");
    EXPECT_TRUE(env->hint_delay == 0, "negative hint delay should clamp to zero");

    previous = ic_set_hint_delay(99999);
    EXPECT_TRUE(previous == 0, "hint delay setter should return prior clamped value");
    EXPECT_TRUE(env->hint_delay == 5000, "hint delay should clamp to documented maximum");

    previous = ic_set_hint_delay(73);
    EXPECT_TRUE(previous == 5000, "hint delay setter should return previous maximum clamp");
    EXPECT_TRUE(env->hint_delay == 73, "hint delay should accept in-range values unchanged");

    return true;
}

static bool test_idle_timeout_setting(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->idle_timeout = 250;
    long previous = ic_set_idle_timeout(-1);
    EXPECT_TRUE(previous == 250, "idle timeout setter should return previous value");
    EXPECT_TRUE(env->idle_timeout == 0, "negative idle timeout should disable the timer");

    previous = ic_set_idle_timeout(1250);
    EXPECT_TRUE(previous == 0, "idle timeout setter should return prior disabled value");
    EXPECT_TRUE(env->idle_timeout == 1250,
                "idle timeout should accept positive millisecond values");

    (void)ic_set_idle_timeout(0);
    return true;
}

static bool test_status_hint_mode_validation(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->status_hint_mode = IC_STATUS_HINT_PERSISTENT;
    // Verify normalization of invalid values at the public C API boundary.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ic_status_hint_mode_t prev = ic_set_status_hint_mode((ic_status_hint_mode_t)99);
    EXPECT_TRUE(prev == IC_STATUS_HINT_PERSISTENT,
                "invalid status hint mode should report previous configured mode");
    EXPECT_TRUE(ic_get_status_hint_mode() == IC_STATUS_HINT_NORMAL,
                "invalid status hint mode should normalize to NORMAL");

    prev = ic_set_status_hint_mode(IC_STATUS_HINT_OFF);
    EXPECT_TRUE(prev == IC_STATUS_HINT_NORMAL,
                "setting OFF should report normalized previous mode");
    EXPECT_TRUE(ic_get_status_hint_mode() == IC_STATUS_HINT_OFF,
                "status hint getter should expose OFF mode");

    (void)ic_set_status_hint_mode(IC_STATUS_HINT_NORMAL);
    return true;
}

static bool test_mouse_reporting_option_toggles(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->mouse_reporting_mode = IC_MOUSE_CLICKING_DISABLED;
    env->mouse_reporting_enabled_by_default = false;

    EXPECT_TRUE(ic_get_mouse_clicking_mode() == IC_MOUSE_CLICKING_DISABLED,
                "mouse mode getter should mirror disabled state");
    EXPECT_TRUE(
        ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_MENU_ONLY) == IC_MOUSE_CLICKING_DISABLED,
        "mouse mode setter should report previous all-off mode");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_MENU_ONLY,
                "mouse mode setter should store menu-only mode");
    EXPECT_FALSE(env->mouse_reporting_enabled_by_default,
                 "menu-only mode should keep editing capture off");

    EXPECT_TRUE(ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_SIMPLE) == IC_MOUSE_CLICKING_MENU_ONLY,
                "mouse mode setter should report previous menu-only mode");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SIMPLE,
                "mouse mode setter should store simple mode");
    EXPECT_TRUE(env->mouse_reporting_enabled_by_default,
                "simple mode should enable mouse capture by default");

    EXPECT_TRUE(ic_set_mouse_clicking_mode(IC_MOUSE_CLICKING_DISABLED) == IC_MOUSE_CLICKING_SIMPLE,
                "mouse mode setter should report previous simple mode");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_DISABLED,
                "mouse mode setter should store disabled mode");
    EXPECT_FALSE(env->mouse_reporting_enabled_by_default,
                 "disabled mode should force startup mouse capture off");

    EXPECT_TRUE(
        ic_set_mouse_clicking_mode((ic_mouse_clicking_mode_t)42) == IC_MOUSE_CLICKING_DISABLED,
        "invalid mouse mode should report previous disabled mode");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART,
                "invalid mouse mode should normalize to smart mode");
    EXPECT_TRUE(env->mouse_reporting_enabled_by_default,
                "smart mode should enable mouse capture by default");

    EXPECT_TRUE(ic_enable_mouse_clicking(false),
                "legacy mouse startup toggle should report previous enabled state");
    EXPECT_FALSE(env->mouse_reporting_enabled_by_default,
                 "legacy mouse startup toggle should disable startup capture");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART,
                "legacy mouse startup toggle should preserve smart mode");

    EXPECT_FALSE(ic_enable_mouse_clicking(true),
                 "legacy mouse startup toggle should report previous disabled state");
    EXPECT_TRUE(env->mouse_reporting_enabled_by_default,
                "legacy mouse startup toggle should enable startup capture");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART,
                "legacy mouse startup toggle should keep smart mode when already enabled");

    env->mouse_reporting_mode = IC_MOUSE_CLICKING_DISABLED;
    env->mouse_reporting_enabled_by_default = false;
    EXPECT_FALSE(ic_enable_mouse_clicking(true),
                 "legacy enable should report previous disabled startup state");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SIMPLE,
                "legacy enable should lift disabled mode to simple mode");
    EXPECT_TRUE(env->mouse_reporting_enabled_by_default,
                "legacy enable should turn startup mouse capture back on");

    env->mouse_reporting_mode = IC_MOUSE_CLICKING_MENU_ONLY;
    env->mouse_reporting_enabled_by_default = false;
    EXPECT_FALSE(ic_enable_mouse_clicking(true),
                 "legacy enable should report previous menu-only editing state");
    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_SIMPLE,
                "legacy enable should lift menu-only mode to simple mode");
    EXPECT_TRUE(env->mouse_reporting_enabled_by_default,
                "legacy enable should activate editing capture from menu-only mode");

    env->mouse_reporting_status_line_enabled = true;
    EXPECT_TRUE(ic_enable_mouse_reporting_status_line(false),
                "mouse status-line toggle should report previous enabled state");
    EXPECT_FALSE(env->mouse_reporting_status_line_enabled,
                 "mouse status-line toggle should hide the mouse indicator line");
    EXPECT_FALSE(ic_enable_mouse_reporting_status_line(true),
                 "mouse status-line toggle should report previous disabled state");
    EXPECT_TRUE(env->mouse_reporting_status_line_enabled,
                "mouse status-line toggle should restore the indicator line");

    return true;
}

static bool test_mouse_reporting_defaults(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    EXPECT_TRUE(env->mouse_reporting_mode == IC_MOUSE_CLICKING_MENU_ONLY,
                "mouse clicking should default to menu-only off mode");
    EXPECT_FALSE(env->mouse_reporting_enabled_by_default,
                 "menu-only default should leave editing capture disabled");
    return true;
}

static bool test_option_toggle_consistency(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    EXPECT_FALSE(ic_completion_auto_menu_is_enabled(),
                 "automatic completion menu should default to disabled");
    EXPECT_FALSE(ic_enable_completion_auto_menu(true),
                 "automatic menu enable should report previously disabled state");
    EXPECT_TRUE(ic_completion_auto_menu_is_enabled(),
                "automatic menu getter should reflect enable");
    EXPECT_TRUE(env->completion_auto_menu, "automatic menu flag should become enabled");
    EXPECT_TRUE(ic_enable_completion_auto_menu(false),
                "automatic menu disable should report previously enabled state");
    EXPECT_FALSE(ic_completion_auto_menu_is_enabled(),
                 "automatic menu getter should reflect disable");

    env->complete_autotab = false;
    EXPECT_FALSE(ic_enable_auto_tab(true), "auto-tab should report previously disabled state");
    EXPECT_TRUE(env->complete_autotab, "auto-tab flag should become enabled");

    env->complete_nopreview = false;
    EXPECT_TRUE(ic_enable_completion_preview(false),
                "completion preview disable should report previously enabled state");
    EXPECT_TRUE(env->complete_nopreview, "preview disable should invert internal flag");
    EXPECT_FALSE(ic_enable_completion_preview(true),
                 "completion preview enable should report previously disabled state");
    EXPECT_FALSE(env->complete_nopreview, "preview enable should clear inverted flag");

    env->menu_highlight_mode = IC_MENU_HIGHLIGHT_NONE;
    EXPECT_TRUE(ic_get_menu_highlight_mode() == IC_MENU_HIGHLIGHT_NONE,
                "menu highlighting should default to none");
    EXPECT_TRUE(ic_set_menu_highlight_mode(IC_MENU_HIGHLIGHT_SINGLE) == IC_MENU_HIGHLIGHT_NONE,
                "menu highlighting setter should report previous none mode");
    EXPECT_TRUE(ic_get_menu_highlight_mode() == IC_MENU_HIGHLIGHT_SINGLE,
                "menu highlighting getter should report single mode");
    EXPECT_TRUE(ic_set_menu_highlight_mode(IC_MENU_HIGHLIGHT_ALL) == IC_MENU_HIGHLIGHT_SINGLE,
                "menu highlighting setter should report previous single mode");
    EXPECT_TRUE(ic_get_menu_highlight_mode() == IC_MENU_HIGHLIGHT_ALL,
                "menu highlighting getter should report all mode");
    EXPECT_TRUE(ic_set_menu_highlight_mode(IC_MENU_HIGHLIGHT_REVERSE) == IC_MENU_HIGHLIGHT_ALL,
                "menu highlighting setter should report previous all mode");
    EXPECT_TRUE(ic_get_menu_highlight_mode() == IC_MENU_HIGHLIGHT_REVERSE,
                "menu highlighting getter should report reverse mode");
    EXPECT_TRUE(
        ic_set_menu_highlight_mode((ic_menu_highlight_mode_t)999) == IC_MENU_HIGHLIGHT_REVERSE,
        "invalid menu highlighting mode should report previous mode");
    EXPECT_TRUE(ic_get_menu_highlight_mode() == IC_MENU_HIGHLIGHT_NONE,
                "invalid menu highlighting mode should reset to none");

    env->no_multiline_indent = false;
    EXPECT_TRUE(ic_enable_multiline_indent(false),
                "multiline indent disable should report previously enabled state");
    EXPECT_TRUE(env->no_multiline_indent, "multiline indent disable should set inverted flag");

    env->no_hint = false;
    EXPECT_TRUE(ic_enable_hint(false), "hint disable should report previously enabled state");
    EXPECT_TRUE(env->no_hint, "hint disable should set inverted no_hint flag");

    env->spell_correct = true;
    EXPECT_TRUE(ic_enable_spell_correct(false),
                "spell-correct disable should report previously enabled state");
    EXPECT_FALSE(env->spell_correct, "spell-correct disable should clear flag");

    env->spell_correct_on_enter = false;
    EXPECT_FALSE(ic_enable_spell_correct_on_enter(true),
                 "spell-correct-on-enter enable should report previously disabled state");
    EXPECT_TRUE(env->spell_correct_on_enter,
                "spell-correct-on-enter enable should set environment flag");
    EXPECT_TRUE(ic_enable_spell_correct_on_enter(false),
                "spell-correct-on-enter disable should report previously enabled state");
    EXPECT_FALSE(env->spell_correct_on_enter,
                 "spell-correct-on-enter disable should clear environment flag");

    env->no_highlight = false;
    EXPECT_TRUE(ic_enable_highlight(false),
                "highlight disable should report previously enabled state");
    EXPECT_TRUE(env->no_highlight, "highlight disable should set inverted flag");

    env->no_help = false;
    EXPECT_TRUE(ic_enable_inline_help(false),
                "inline help disable should report previously enabled state");
    EXPECT_TRUE(env->no_help, "inline help disable should set inverted flag");

    env->highlight_current_line_number = true;
    EXPECT_TRUE(ic_enable_current_line_number_highlight(false),
                "current line number highlight disable should report previous state");
    EXPECT_FALSE(ic_current_line_number_highlight_is_enabled(),
                 "current line number highlight getter should mirror state");

    env->inline_right_prompt_follows_cursor = false;
    EXPECT_FALSE(ic_enable_inline_right_prompt_cursor_follow(true),
                 "inline right prompt follow should report previous disabled state");
    EXPECT_TRUE(ic_inline_right_prompt_follows_cursor(),
                "inline right prompt follow getter should mirror state");

    env->no_bracematch = false;
    EXPECT_TRUE(ic_enable_brace_matching(false),
                "brace matching disable should report previously enabled state");
    EXPECT_TRUE(env->no_bracematch, "brace matching disable should set inverted flag");

    env->no_autobrace = false;
    EXPECT_TRUE(ic_enable_brace_insertion(false),
                "brace insertion disable should report previously enabled state");
    EXPECT_TRUE(env->no_autobrace, "brace insertion disable should set inverted flag");

    (void)ic_enable_multiline_indent(true);
    (void)ic_enable_hint(true);
    (void)ic_enable_spell_correct(true);
    (void)ic_enable_spell_correct_on_enter(false);
    (void)ic_enable_highlight(true);
    (void)ic_enable_inline_help(true);
    (void)ic_enable_current_line_number_highlight(true);
    (void)ic_enable_inline_right_prompt_cursor_follow(false);
    (void)ic_enable_brace_matching(true);
    (void)ic_enable_brace_insertion(true);
    (void)ic_set_menu_highlight_mode(IC_MENU_HIGHLIGHT_NONE);
    return true;
}

static bool test_brace_pair_setters_validation(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_set_matching_braces("()<>[]");
    EXPECT_STREQ(ic_env_get_match_braces(env), "()<>[]",
                 "even-length matching brace string should be stored as-is");

    ic_set_matching_braces("(");
    EXPECT_STREQ(ic_env_get_match_braces(env), "()[]{}",
                 "odd-length matching brace string should fall back to default");

    ic_set_matching_braces(NULL);
    EXPECT_STREQ(ic_env_get_match_braces(env), "()[]{}",
                 "NULL matching brace string should fall back to default");

    ic_set_insertion_braces("{}\"\"");
    EXPECT_STREQ(ic_env_get_auto_braces(env), "{}\"\"",
                 "even-length insertion brace string should be stored as-is");

    ic_set_insertion_braces("{");
    EXPECT_STREQ(ic_env_get_auto_braces(env), "()[]{}\"\"''",
                 "odd-length insertion brace string should fall back to default");

    ic_set_insertion_braces(NULL);
    EXPECT_STREQ(ic_env_get_auto_braces(env), "()[]{}\"\"''",
                 "NULL insertion brace string should fall back to default");

    return true;
}

static bool test_abbreviation_management(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_clear_abbreviations();
    EXPECT_TRUE(env->abbreviation_count == 0, "clear abbreviations should reset entry count");

    EXPECT_FALSE(ic_add_abbreviation(NULL, "noop"), "NULL trigger should be rejected");
    EXPECT_FALSE(ic_add_abbreviation("", "noop"), "empty trigger should be rejected");
    EXPECT_FALSE(ic_add_abbreviation("bad key", "noop"),
                 "whitespace in trigger should be rejected");
    EXPECT_FALSE(ic_add_abbreviation("ok", NULL), "NULL expansion should be rejected");

    EXPECT_TRUE(ic_add_abbreviation("gc", "git commit"), "valid abbreviation should be inserted");
    EXPECT_TRUE(env->abbreviation_count == 1, "abbreviation count should increase after insertion");
    EXPECT_STREQ(env->abbreviations[0].trigger, "gc", "trigger should be copied into env storage");
    EXPECT_STREQ(env->abbreviations[0].expansion, "git commit",
                 "expansion should be copied into env storage");

    EXPECT_TRUE(ic_add_abbreviation("gc", "git commit --verbose"),
                "adding existing trigger should update expansion in-place");
    EXPECT_TRUE(env->abbreviation_count == 1,
                "updating an existing trigger should not duplicate entries");
    EXPECT_STREQ(env->abbreviations[0].expansion, "git commit --verbose",
                 "existing expansion should be replaced when trigger already exists");

    EXPECT_FALSE(ic_remove_abbreviation("missing"),
                 "removing unknown trigger should report failure");
    EXPECT_TRUE(ic_remove_abbreviation("gc"), "removing existing trigger should succeed");
    EXPECT_TRUE(env->abbreviation_count == 0,
                "abbreviation count should decrease after successful removal");

    EXPECT_TRUE(ic_add_abbreviation("ga", "git add"), "should allow adding first abbreviation");
    EXPECT_TRUE(ic_add_abbreviation("gs", "git status"), "should allow adding second abbreviation");
    ic_clear_abbreviations();
    EXPECT_TRUE(env->abbreviation_count == 0 && env->abbreviation_capacity == 0,
                "clear should reset abbreviation storage metadata");
    EXPECT_TRUE(env->abbreviations == NULL, "clear should release abbreviation array");

    return true;
}

static bool test_key_spec_parse_and_format_roundtrip(void) {
    ic_keycode_t key = IC_KEY_NONE;
    EXPECT_TRUE(ic_parse_key_spec("ctrl+k", &key), "ctrl+k key spec should parse");
    EXPECT_TRUE(key == IC_KEY_CTRL_K, "ctrl+k should map to ASCII control keycode");

    EXPECT_TRUE(ic_parse_key_spec("alt+shift+f12", &key),
                "combined modifier function-key spec should parse");
    EXPECT_TRUE(key == IC_KEY_WITH_ALT(IC_KEY_WITH_SHIFT(IC_KEY_F12)),
                "combined modifiers should be encoded in keycode flags");

    EXPECT_TRUE(ic_parse_key_spec("ctrl+@", &key), "ctrl+@ should parse as ctrl-space shortcut");
    EXPECT_TRUE(key == IC_KEY_CTRL_SPACE, "ctrl+@ should map to ctrl-space keycode");

    EXPECT_FALSE(ic_parse_key_spec("ctrl+alt", &key), "modifier-only key spec should be rejected");
    EXPECT_FALSE(ic_parse_key_spec("definitely-not-a-key", &key),
                 "unknown key token should be rejected");

    char formatted[64];
    EXPECT_TRUE(ic_format_key_spec(IC_KEY_CTRL_K, formatted, sizeof(formatted)),
                "formatting ctrl-k should succeed");
    EXPECT_STREQ(formatted, "ctrl+k", "formatted ctrl-k key spec should be canonicalized");

    EXPECT_TRUE(ic_format_key_spec(IC_KEY_WITH_ALT(IC_KEY_F12), formatted, sizeof(formatted)),
                "formatting alt+f12 should succeed");
    EXPECT_STREQ(formatted, "alt+f12", "formatted alt+f12 key spec should be canonicalized");

    EXPECT_TRUE(ic_format_key_spec(IC_KEY_NONE, formatted, sizeof(formatted)),
                "formatting IC_KEY_NONE should succeed");
    EXPECT_STREQ(formatted, "none", "none keycode should format to literal 'none'");

    char tiny[4];
    EXPECT_FALSE(ic_format_key_spec(IC_KEY_WITH_ALT(IC_KEY_F12), tiny, sizeof(tiny)),
                 "format should fail when output buffer is too small");

    return true;
}

static bool test_key_binding_crud_and_profiles(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    const char* original_profile = ic_get_key_binding_profile();
    EXPECT_TRUE(ic_set_key_binding_profile("emacs"), "switching to emacs profile should succeed");

    EXPECT_TRUE(ic_bind_key(IC_KEY_F2, IC_KEY_ACTION_CLEAR_SCREEN),
                "binding explicit key/action should succeed");
    ic_key_action_t action = IC_KEY_ACTION_NONE;
    EXPECT_TRUE(ic_get_key_binding(IC_KEY_F2, &action), "new key binding should be queryable");
    EXPECT_TRUE(action == IC_KEY_ACTION_CLEAR_SCREEN,
                "queried key binding action should match bound value");

    size_t listed_count = ic_list_key_bindings(NULL, 0);
    EXPECT_TRUE(listed_count >= 1, "binding list count should reflect at least one binding");

    ic_key_binding_entry_t entries[64];
    size_t written = ic_list_key_bindings(entries, 64);
    EXPECT_TRUE(written > 0 && written <= listed_count,
                "binding listing with buffer should write bounded number of entries");

    EXPECT_TRUE(ic_clear_key_binding(IC_KEY_F2), "clearing existing binding should succeed");
    EXPECT_FALSE(ic_clear_key_binding(IC_KEY_F2),
                 "clearing same binding twice should fail on second attempt");
    EXPECT_FALSE(ic_bind_key(IC_KEY_F3, IC_KEY_ACTION__MAX),
                 "binding invalid action enum should fail");

    EXPECT_TRUE(ic_bind_key_named("ctrl+x", "undo"),
                "binding named action should accept known aliases");
    EXPECT_TRUE(ic_get_key_binding(IC_KEY_CTRL_X, &action),
                "named binding should resolve to parsed keycode");
    EXPECT_TRUE(action == IC_KEY_ACTION_UNDO,
                "named binding should map alias to expected action enum");

    EXPECT_TRUE(ic_bind_key_named("alt+.", "yank-last-arg"),
                "binding alt+. to yank-last-arg should succeed");
    EXPECT_TRUE(ic_get_key_binding(IC_KEY_WITH_ALT(ic_key_char('.')), &action),
                "explicit alt+. yank-last-arg binding should be queryable");
    EXPECT_TRUE(action == IC_KEY_ACTION_YANK_LAST_ARG,
                "alt+. should resolve to the yank-last-arg action");

    EXPECT_TRUE(ic_bind_key_named("alt+_", "insert-last-argument"),
                "binding alt+_ to insert-last-argument should succeed");
    EXPECT_TRUE(ic_get_key_binding(IC_KEY_WITH_ALT(ic_key_char('_')), &action),
                "explicit alt+_ yank-last-arg binding should be queryable");
    EXPECT_TRUE(action == IC_KEY_ACTION_YANK_LAST_ARG,
                "alt+_ should resolve to the yank-last-arg action");

    size_t profile_count = ic_list_key_binding_profiles(NULL, 0);
    EXPECT_TRUE(profile_count >= 2, "at least emacs and vim profiles should be registered");

    ic_key_binding_profile_info_t profiles[8];
    size_t profile_written = ic_list_key_binding_profiles(profiles, 8);
    bool saw_emacs = false;
    bool saw_vim = false;
    for (size_t i = 0; i < profile_written; ++i) {
        if (profiles[i].name == NULL) {
            continue;
        }
        if (strcmp(profiles[i].name, "emacs") == 0) {
            saw_emacs = true;
        }
        if (strcmp(profiles[i].name, "vim") == 0) {
            saw_vim = true;
        }
    }
    EXPECT_TRUE(saw_emacs && saw_vim, "profile listing should expose both emacs and vim profiles");

    EXPECT_TRUE(ic_set_key_binding_profile("vim"), "switching to vim profile should succeed");
    EXPECT_STREQ(ic_get_key_binding_profile(), "vim",
                 "getter should report newly selected profile");

    const char* default_specs =
        ic_key_binding_profile_default_specs(IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE);
    EXPECT_TRUE(default_specs != NULL && strstr(default_specs, "alt+w") != NULL,
                "vim profile should override cursor-word-next default specs with alt+w");

    const char* yank_specs = ic_key_binding_profile_default_specs(IC_KEY_ACTION_YANK_LAST_ARG);
    EXPECT_TRUE(yank_specs != NULL && strstr(yank_specs, "alt+.") != NULL &&
                    strstr(yank_specs, "alt+_") != NULL,
                "default yank-last-arg specs should expose both readline-style Meta-. and Meta-_");

    const char* mouse_toggle_specs =
        ic_key_binding_profile_default_specs(IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING);
    EXPECT_TRUE(mouse_toggle_specs != NULL && strstr(mouse_toggle_specs, "f2") != NULL,
                "default mouse-toggle specs should expose F2");

    const char* command_palette_specs =
        ic_key_binding_profile_default_specs(IC_KEY_ACTION_COMMAND_PALETTE);
    EXPECT_TRUE(command_palette_specs != NULL && strstr(command_palette_specs, "alt+p") != NULL,
                "default command-palette specs should expose Alt+P");

    EXPECT_FALSE(ic_set_key_binding_profile("does-not-exist"),
                 "unknown key binding profile should be rejected");

    EXPECT_TRUE(ic_set_key_binding_profile(original_profile),
                "restoring original key binding profile should succeed");
    return true;
}

static bool profile_specs_contain_token(const char* specs, const char* expected_token) {
    if (specs == NULL || expected_token == NULL) {
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
        while (start < end && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        if (start >= end) {
            continue;
        }

        size_t len = (size_t)(end - start);
        if (strlen(expected_token) == len && strncmp(start, expected_token, len) == 0) {
            return true;
        }
    }

    return false;
}

static bool expect_profile_spec_tokens_roundtrip(const char* profile_name, ic_key_action_t action,
                                                 const char* specs, size_t* token_count_out) {
    if (token_count_out != NULL) {
        *token_count_out = 0;
    }
    if (profile_name == NULL || specs == NULL) {
        return false;
    }

    const char* action_name = ic_key_action_name(action);
    if (action_name == NULL) {
        action_name = "(unknown)";
    }

    size_t token_count = 0;
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
        while (start < end && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        if (start >= end) {
            continue;
        }

        size_t len = (size_t)(end - start);
        char token[64];
        if (len >= sizeof(token)) {
            expect_safe_log("Profile '%s' action '%s' contains oversized key spec token\n",
                            profile_name, action_name);
            return false;
        }

        memcpy(token, start, len);
        token[len] = '\0';

        ic_keycode_t key = IC_KEY_NONE;
        if (!ic_parse_key_spec(token, &key)) {
            expect_safe_log("Profile '%s' action '%s' failed to parse key spec '%s'\n",
                            profile_name, action_name, token);
            return false;
        }

        char formatted[64];
        if (!ic_format_key_spec(key, formatted, sizeof(formatted))) {
            expect_safe_log("Profile '%s' action '%s' failed to format key spec '%s'\n",
                            profile_name, action_name, token);
            return false;
        }

        token_count++;
    }

    if (token_count == 0) {
        expect_safe_log("Profile '%s' action '%s' did not expose any key spec tokens\n",
                        profile_name, action_name);
        return false;
    }

    if (token_count_out != NULL) {
        *token_count_out = token_count;
    }
    return true;
}

typedef struct explicit_profile_binding_s {
    const char* profile_name;
    const char* key_spec;
    ic_key_action_t action;
} explicit_profile_binding_t;

static bool expect_explicit_profile_binding(const explicit_profile_binding_t* binding) {
    if (binding == NULL) {
        return false;
    }

    ic_keycode_t key = IC_KEY_NONE;
    if (!ic_parse_key_spec(binding->key_spec, &key)) {
        expect_safe_log("explicit profile binding '%s' failed to parse\n", binding->key_spec);
        return false;
    }

    ic_key_action_t bound_action = IC_KEY_ACTION_NONE;
    if (!ic_get_key_binding(key, &bound_action)) {
        expect_safe_log("profile '%s' did not register explicit binding '%s'\n",
                        binding->profile_name, binding->key_spec);
        return false;
    }

    if (bound_action != binding->action) {
        expect_safe_log(
            "profile '%s' binding '%s' resolved to '%s' instead of '%s'\n", binding->profile_name,
            binding->key_spec,
            (ic_key_action_name(bound_action) != NULL ? ic_key_action_name(bound_action)
                                                      : "(unknown)"),
            (ic_key_action_name(binding->action) != NULL ? ic_key_action_name(binding->action)
                                                         : "(unknown)"));
        return false;
    }

    const char* specs = ic_key_binding_profile_default_specs(binding->action);
    if (!profile_specs_contain_token(specs, binding->key_spec)) {
        expect_safe_log("profile '%s' binding '%s' missing from documented specs\n",
                        binding->profile_name, binding->key_spec);
        return false;
    }

    return true;
}

static bool test_key_binding_profile_specs_register_all_bindings(void) {
    const char* original_profile = ic_get_key_binding_profile();
    static const char* const profiles[] = {"emacs", "vim"};
    static const explicit_profile_binding_t explicit_bindings[] = {
        {"vim", "alt+h", IC_KEY_ACTION_CURSOR_LEFT},
        {"vim", "alt+j", IC_KEY_ACTION_CURSOR_DOWN},
        {"vim", "alt+k", IC_KEY_ACTION_CURSOR_UP},
        {"vim", "alt+l", IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE},
        {"vim", "alt+w", IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE},
    };

    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        const char* profile_name = profiles[i];
        if (!ic_set_key_binding_profile(profile_name)) {
            expect_safe_log("failed to activate key binding profile '%s'\n", profile_name);
            return false;
        }

        size_t expected_binding_count = 0;
        size_t action_count = 0;
        for (int action = IC_KEY_ACTION_NONE + 1; action < IC_KEY_ACTION__MAX; ++action) {
            const char* specs = ic_key_binding_profile_default_specs((ic_key_action_t)action);
            if (specs == NULL || specs[0] == '\0') {
                continue;
            }

            size_t token_count = 0;
            if (!expect_profile_spec_tokens_roundtrip(profile_name, (ic_key_action_t)action, specs,
                                                      &token_count)) {
                return false;
            }
            expected_binding_count += token_count;
            action_count++;
        }

        if (action_count == 0) {
            expect_safe_log("profile '%s' exposed no default key binding specs\n", profile_name);
            return false;
        }

        size_t expected_explicit_binding_count = 0;
        for (size_t binding_idx = 0;
             binding_idx < sizeof(explicit_bindings) / sizeof(explicit_bindings[0]);
             ++binding_idx) {
            if (strcmp(explicit_bindings[binding_idx].profile_name, profile_name) == 0) {
                expected_explicit_binding_count++;
                if (!expect_explicit_profile_binding(&explicit_bindings[binding_idx])) {
                    return false;
                }
            }
        }

        size_t listed_count = ic_list_key_bindings(NULL, 0);
        if (listed_count != expected_explicit_binding_count) {
            expect_safe_log(
                "profile '%s' documented %zu spec tokens but registered %zu explicit bindings "
                "instead of %zu\n",
                profile_name, expected_binding_count, listed_count,
                expected_explicit_binding_count);
            return false;
        }
    }

    if (!ic_set_key_binding_profile(original_profile)) {
        expect_safe_log("failed to restore original key binding profile '%s'\n", original_profile);
        return false;
    }
    return true;
}

static bool test_key_action_name_mappings(void) {
    EXPECT_TRUE(ic_key_action_from_name("history-up") == IC_KEY_ACTION_HISTORY_PREV,
                "history-up alias should map to HISTORY_PREV action");
    EXPECT_TRUE(ic_key_action_from_name("completion") == IC_KEY_ACTION_COMPLETE,
                "completion alias should map to COMPLETE action");
    EXPECT_TRUE(ic_key_action_from_name("insert-last-argument") == IC_KEY_ACTION_YANK_LAST_ARG,
                "insert-last-argument alias should map to YANK_LAST_ARG action");
    EXPECT_TRUE(ic_key_action_from_name("toggle-mouse") == IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING,
                "toggle-mouse alias should map to TOGGLE_MOUSE_REPORTING action");
    EXPECT_TRUE(ic_key_action_from_name("palette") == IC_KEY_ACTION_COMMAND_PALETTE,
                "palette alias should map to COMMAND_PALETTE action");
    EXPECT_TRUE(ic_key_action_from_name("unhandled") == IC_KEY_ACTION_RUNOFF,
                "unhandled alias should map to RUNOFF action");
    EXPECT_TRUE(ic_key_action_from_name("unknown-action") == IC_KEY_ACTION__MAX,
                "unknown action name should map to sentinel");

    EXPECT_STREQ(ic_key_action_name(IC_KEY_ACTION_YANK_LAST_ARG), "yank-last-arg",
                 "action-to-name lookup should return canonical yank-last-arg label");
    EXPECT_STREQ(ic_key_action_name(IC_KEY_ACTION_CLEAR_SCREEN), "clear-screen",
                 "action-to-name lookup should return canonical clear-screen label");
    EXPECT_STREQ(ic_key_action_name(IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING), "toggle-mouse-reporting",
                 "action-to-name lookup should return canonical toggle-mouse-reporting label");
    EXPECT_STREQ(ic_key_action_name(IC_KEY_ACTION_COMMAND_PALETTE), "command-palette",
                 "action-to-name lookup should return canonical command-palette label");
    EXPECT_TRUE(ic_key_action_name(IC_KEY_ACTION__MAX) == NULL,
                "invalid action enum should not produce a name");

    return true;
}

static bool test_string_copy_bounds(void) {
    struct {
        char text[4];
        char guard;
    } buffer = {{'?', '?', '?', '\0'}, '#'};

    EXPECT_TRUE(ic_strcpy(buffer.text, sizeof(buffer.text), "abc"),
                "an exact-fit string should include its terminator");
    EXPECT_STREQ(buffer.text, "abc", "exact-fit copy should preserve the text");
    EXPECT_TRUE(buffer.guard == '#', "copy should leave the next byte untouched");

    EXPECT_TRUE(!ic_strcpy(buffer.text, sizeof(buffer.text), "abcd"),
                "an oversized string should be rejected");
    EXPECT_STREQ(buffer.text, "abc", "a rejected copy should leave the destination unchanged");
    EXPECT_TRUE(!ic_strcpy(buffer.text, 0, ""), "a zero-capacity destination should be rejected");
    EXPECT_TRUE(ic_strcpy(buffer.text, sizeof(buffer.text), ""), "an empty string should fit");
    EXPECT_TRUE(buffer.text[0] == '\0' && buffer.text[1] == 'b' && buffer.guard == '#',
                "an empty copy should write only its terminator");
    return true;
}

static bool test_string_matching_and_token_helpers(void) {
    EXPECT_TRUE(ic_starts_with("prefix-value", "pre"),
                "starts_with should match explicit ASCII prefixes");
    EXPECT_FALSE(ic_starts_with("prefix-value", "Prefix"), "starts_with should be case-sensitive");
    EXPECT_TRUE(ic_starts_with("anything", NULL), "NULL prefix should be treated as empty prefix");
    EXPECT_FALSE(ic_starts_with(NULL, "x"),
                 "NULL source string should never match non-NULL prefix");

    EXPECT_TRUE(ic_istarts_with("Prefix-Value", "pre"),
                "istarts_with should match ASCII prefixes case-insensitively");
    EXPECT_FALSE(ic_istarts_with(NULL, "pre"),
                 "istarts_with should fail when source string is NULL");

    EXPECT_TRUE(ic_char_is_white(" ", 1), "space should classify as whitespace");
    EXPECT_TRUE(ic_char_is_nonwhite("x", 1), "regular letters should classify as non-whitespace");
    EXPECT_TRUE(ic_char_is_separator(";", 1), "semicolon should classify as separator");
    EXPECT_TRUE(ic_char_is_nonseparator("a", 1), "letters should classify as non-separators");
    EXPECT_TRUE(ic_char_is_digit("9", 1), "digit classification should recognize decimal digits");
    EXPECT_TRUE(ic_char_is_hexdigit("F", 1),
                "hexdigit classification should recognize uppercase hexadecimal digits");
    EXPECT_TRUE(ic_char_is_letter("Z", 1), "letter classification should recognize ASCII letters");
    EXPECT_TRUE(ic_char_is_idletter("_", 1), "idletter classification should include underscore");
    EXPECT_TRUE(ic_char_is_filename_letter(".", 1),
                "filename letter classification should include period character");

    const char* sample = "func test";
    EXPECT_TRUE(ic_is_token(sample, 0, &ic_char_is_letter) == 4,
                "token detector should report token length at token boundaries");
    EXPECT_TRUE(ic_is_token(sample, 1, &ic_char_is_letter) == -1,
                "token detector should reject offsets inside an existing token");
    EXPECT_TRUE(ic_match_token(sample, 0, &ic_char_is_letter, "func") == 4,
                "match_token should return token length on exact match");
    EXPECT_TRUE(ic_match_token(sample, 0, &ic_char_is_letter, "fun") == 0,
                "match_token should reject strict prefix matches");

    const char* tokens[] = {"foo", "func", NULL};
    EXPECT_TRUE(ic_match_any_token(sample, 0, &ic_char_is_letter, tokens) == 4,
                "match_any_token should return matching token length when any candidate matches");
    EXPECT_TRUE(ic_match_any_token(sample, 5, &ic_char_is_letter, tokens) == 0,
                "match_any_token should return zero when no candidate matches");

    return true;
}

static bool test_prev_next_char_utf8_helpers(void) {
    const char sample[] = {'a', (char)0xE2, (char)0x82, (char)0xAC, 'b', 0};

    EXPECT_TRUE(ic_next_char(sample, 0) == 1, "next_char should advance one byte over ASCII");
    EXPECT_TRUE(ic_next_char(sample, 1) == 4,
                "next_char should advance over complete multi-byte UTF-8 sequence");
    EXPECT_TRUE(ic_next_char(sample, 4) == 5, "next_char should advance over trailing ASCII");

    EXPECT_TRUE(ic_prev_char(sample, 5) == 4, "prev_char should step back one byte over ASCII");
    EXPECT_TRUE(ic_prev_char(sample, 4) == 1,
                "prev_char should step back over complete multi-byte UTF-8 sequence");
    EXPECT_TRUE(ic_prev_char(sample, 1) == 0, "prev_char should step back to start of buffer");

    EXPECT_TRUE(ic_next_char(sample, 5) == -1, "next_char should fail at end-of-string cursor");
    EXPECT_TRUE(ic_prev_char(sample, 0) == -1, "prev_char should fail at start-of-string cursor");
    EXPECT_TRUE(ic_next_char(NULL, 0) == -1, "next_char should reject NULL strings");
    EXPECT_TRUE(ic_prev_char(NULL, 0) == -1, "prev_char should reject NULL strings");

    return true;
}

static bool test_term_visibility_tracking_with_escape_and_control_bytes(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    const char* sgr_only = "\x1B[31m";
    term_write_n(env->term, sgr_only, (ssize_t)strlen(sgr_only));
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "ANSI SGR escape bytes alone should not mark line as visibly non-empty");

    term_write_n(env->term, "\t", 1);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "tab should count as visible line content while tracking output");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline should reset tracked visible-content state for the line");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_escape_only_sequences(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    const char* escape_only_sequences[] = {
        "\x1B[31m", "\x1B[0m", "\x1B[2K", "\x1B]0;isocline-title\x07", "\x1B[?2004h", "\x1B[?2004l",
    };

    for (size_t i = 0; i < sizeof(escape_only_sequences) / sizeof(escape_only_sequences[0]); ++i) {
        term_write_n(env->term, escape_only_sequences[i],
                     (ssize_t)strlen(escape_only_sequences[i]));
        EXPECT_FALSE(term_line_has_visible_content(env->term),
                     "escape sequences without printable bytes should not mark line visible");
    }

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_control_bytes_only(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    const char controls[] = {'\a', '\b', '\r', '\v', '\f'};
    term_write_n(env->term, controls, (ssize_t)sizeof(controls));
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "bell/backspace/carriage-return/VT/form-feed should not mark line visible");

    term_write_n(env->term, "X", 1);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "printable bytes after control bytes should mark line visible");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline should reset visibility state after printable content");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_carriage_return_preserves_visible_state(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    term_write_n(env->term, "abc", 3);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "printing ASCII text should mark current line as visible");

    term_write_n(env->term, "\r", 1);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "carriage return alone should not clear previously visible line content");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline after carriage return should clear tracked visible state");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_bracketed_paste_toggle_sequences(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    const char* enable_paste = "\x1B[?2004h";
    const char* disable_paste = "\x1B[?2004l";
    term_write_n(env->term, enable_paste, (ssize_t)strlen(enable_paste));
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "enabling bracketed paste should not count as visible output");

    term_write_n(env->term, disable_paste, (ssize_t)strlen(disable_paste));
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "disabling bracketed paste should not count as visible output");

    term_write_n(env->term, "%", 1);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "printable marker after paste toggles should still mark line visible");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline should reset visibility after printable marker");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_multiline_last_line_only(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    term_write_n(env->term, "alpha\nbeta\n", 11);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "line visibility should reset when output ends with a newline");

    term_write_n(env->term, "gamma", 5);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "visible bytes on the final line should mark the line as visibly non-empty");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline should clear tracked visibility after multiline writes");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_cursor_start_tracking_transitions(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    term_write_n(env->term, "\r", 1);
    EXPECT_TRUE(term_is_cursor_at_line_start(env->term),
                "carriage return should initialize tracked cursor at line start");

    term_write_n(env->term, "abc", 3);
    EXPECT_FALSE(term_is_cursor_at_line_start(env->term),
                 "printing visible text should move tracked cursor away from line start");

    term_write_n(env->term, "\r", 1);
    EXPECT_TRUE(term_is_cursor_at_line_start(env->term),
                "carriage return should restore tracked cursor to line start");

    term_write_n(env->term, "\t", 1);
    EXPECT_FALSE(term_is_cursor_at_line_start(env->term),
                 "tab should move tracked cursor away from line start");

    term_write_n(env->term, "\n", 1);
    EXPECT_TRUE(term_is_cursor_at_line_start(env->term),
                "newline should reset tracked cursor to line start for the next row");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_term_visibility_tracking_escape_whitespace_mix(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    term_set_track_output(env->term, true);

    const char* colored_space = "\x1B[31m \x1B[0m";
    term_write_n(env->term, colored_space, (ssize_t)strlen(colored_space));
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "printable whitespace wrapped in ANSI escapes should still count as visible");

    term_write_n(env->term, "\n", 1);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "newline should reset visibility state after escaped whitespace");

    const char* escape_only = "\x1B[35m\x1B[0m";
    term_write_n(env->term, escape_only, (ssize_t)strlen(escape_only));
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "escape-only output should remain invisible after prior newline reset");

    term_set_track_output(env->term, false);
    term_reset_line_state(env->term);
    return true;
}

static bool test_tty_bracketed_paste_enter_translation_flow(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t raw[] = {'\x1B', '[', '2', '0', '0', '~', '\r',
                                  '\x1B', '[', '2', '0', '1', '~', '\r'};
    EXPECT_TRUE(ic_push_raw_input(raw, sizeof(raw)),
                "raw bracketed paste marker sequence should enqueue into active TTY");

    code_t seen[8];
    size_t seen_count = 0;
    for (size_t i = 0; i < 32 && seen_count < 4; ++i) {
        code_t code = KEY_NONE;
        if (!tty_read_timeout(env->tty, 0, &code)) {
            break;
        }
        if (code == KEY_EVENT_RESIZE) {
            continue;
        }
        seen[seen_count++] = code;
    }

    EXPECT_TRUE(seen_count == 4, "expected bracketed paste marker test to yield four key events");
    EXPECT_TRUE(seen[0] == IC_KEY_PASTE_START,
                "first decoded key should be bracketed paste start event");
    EXPECT_TRUE(seen[1] == KEY_LINEFEED,
                "enter pressed during bracketed paste should decode as linefeed");
    EXPECT_TRUE(seen[2] == IC_KEY_PASTE_END,
                "third decoded key should be bracketed paste end event");
    EXPECT_TRUE(seen[3] == KEY_ENTER,
                "enter pressed after paste end should decode as regular enter");

    return true;
}

static bool test_tty_bracketed_paste_repeated_start_without_end(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t raw[] = {'\x1B', '[', '2', '0',    '0', '~', '\x1B', '[', '2', '0',
                                  '0',    '~', 'A', '\x1B', '[', '2', '0',    '1', '~', '\r'};
    EXPECT_TRUE(ic_push_raw_input(raw, sizeof(raw)),
                "repeated bracketed paste starts should still enqueue successfully");

    code_t seen[10];
    size_t seen_count = 0;
    for (size_t i = 0; i < 40 && seen_count < 5; ++i) {
        code_t code = KEY_NONE;
        if (!tty_read_timeout(env->tty, 0, &code)) {
            break;
        }
        if (code == KEY_EVENT_RESIZE) {
            continue;
        }
        seen[seen_count++] = code;
    }

    EXPECT_TRUE(seen_count == 5,
                "repeated-start sequence should decode into two starts, text, end, and enter");
    EXPECT_TRUE(seen[0] == IC_KEY_PASTE_START, "first event should enter bracketed paste mode");
    EXPECT_TRUE(seen[1] == IC_KEY_PASTE_START,
                "second start marker should still decode as bracketed paste start event");
    EXPECT_TRUE(seen[2] == 'A',
                "printable bytes inside repeated-start paste sequence should remain text input");
    EXPECT_TRUE(seen[3] == IC_KEY_PASTE_END,
                "single end marker should decode and exit bracketed paste mode");
    EXPECT_TRUE(seen[4] == KEY_ENTER, "enter after end marker should decode as regular enter key");

    return true;
}

static bool test_tty_bracketed_paste_repeated_end_without_start(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t raw[] = {'\x1B', '[', '2', '0', '1', '~', '\x1B',
                                  '[',    '2', '0', '1', '~', 'B', '\r'};
    EXPECT_TRUE(ic_push_raw_input(raw, sizeof(raw)),
                "repeated end markers should enqueue into active TTY");

    code_t seen[8];
    size_t seen_count = 0;
    for (size_t i = 0; i < 32 && seen_count < 4; ++i) {
        code_t code = KEY_NONE;
        if (!tty_read_timeout(env->tty, 0, &code)) {
            break;
        }
        if (code == KEY_EVENT_RESIZE) {
            continue;
        }
        seen[seen_count++] = code;
    }

    EXPECT_TRUE(seen_count == 4,
                "repeated end sequence should decode into two ends, text, and trailing enter");
    EXPECT_TRUE(seen[0] == IC_KEY_PASTE_END,
                "first marker should decode as bracketed paste end event");
    EXPECT_TRUE(seen[1] == IC_KEY_PASTE_END,
                "second marker should also decode as bracketed paste end event");
    EXPECT_TRUE(seen[2] == 'B',
                "printable bytes after repeated end markers should decode as normal input");
    EXPECT_TRUE(seen[3] == KEY_ENTER,
                "enter after repeated end markers should remain regular enter");

    return true;
}

static bool test_tty_sgr_mouse_event_metadata(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t seq_left_press[] = {'\x1B', '[', '<', '4', ';', '6', ';', '4', 'M'};
    static const uint8_t seq_left_release[] = {'\x1B', '[', '<', '4', ';', '6', ';', '4', 'm'};
    static const uint8_t seq_wheel_up[] = {'\x1B', '[', '<', '6', '4', ';', '7', ';', '5', 'M'};
    static const uint8_t seq_wheel_down[] = {'\x1B', '[', '<', '6', '5', ';',
                                             '7',    '5', ';', '5', 'M'};
    static const uint8_t seq_left_drag[] = "\x1b[<32;8;4M";
    static const uint8_t seq_modified_drag[] = "\x1b[<52;8;4M";
    static const uint8_t seq_right_drag[] = "\x1b[<34;8;4M";
    static const uint8_t seq_hover[] = "\x1b[<35;8;4M";
    static const uint8_t seq_legacy_drag[] = {'\x1b', '[', 'M', 64, 40, 36};

    static const struct {
        const uint8_t* raw;
        size_t raw_len;
        code_t expected_code;
        tty_mouse_action_t expected_action;
        ssize_t expected_column;
        ssize_t expected_row;
        code_t expected_modifiers;
    } cases[] = {
        {seq_left_press, sizeof(seq_left_press), KEY_EVENT_MOUSE_OTHER | KEY_MOD_SHIFT,
         TTY_MOUSE_ACTION_LEFT_PRESS, 6, 4, KEY_MOD_SHIFT},
        {seq_left_release, sizeof(seq_left_release), KEY_EVENT_MOUSE_OTHER | KEY_MOD_SHIFT,
         TTY_MOUSE_ACTION_LEFT_RELEASE, 6, 4, KEY_MOD_SHIFT},
        {seq_wheel_up, sizeof(seq_wheel_up), KEY_EVENT_MOUSE_WHEEL_UP, TTY_MOUSE_ACTION_WHEEL_UP, 7,
         5, 0},
        {seq_wheel_down, sizeof(seq_wheel_down), KEY_EVENT_MOUSE_WHEEL_DOWN,
         TTY_MOUSE_ACTION_WHEEL_DOWN, 75, 5, 0},
        {seq_left_drag, sizeof(seq_left_drag) - 1, KEY_EVENT_MOUSE_OTHER,
         TTY_MOUSE_ACTION_LEFT_DRAG, 8, 4, 0},
        {seq_modified_drag, sizeof(seq_modified_drag) - 1,
         KEY_EVENT_MOUSE_OTHER | KEY_MOD_SHIFT | KEY_MOD_CTRL, TTY_MOUSE_ACTION_LEFT_DRAG, 8, 4,
         KEY_MOD_SHIFT | KEY_MOD_CTRL},
        {seq_right_drag, sizeof(seq_right_drag) - 1, KEY_EVENT_MOUSE_OTHER, TTY_MOUSE_ACTION_OTHER,
         8, 4, 0},
        {seq_hover, sizeof(seq_hover) - 1, KEY_EVENT_MOUSE_OTHER, TTY_MOUSE_ACTION_OTHER, 8, 4, 0},
        {seq_legacy_drag, sizeof(seq_legacy_drag), KEY_EVENT_MOUSE_OTHER,
         TTY_MOUSE_ACTION_LEFT_DRAG, 8, 4, 0}};

    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i) {
        EXPECT_TRUE(ic_push_raw_input(cases[i].raw, cases[i].raw_len),
                    "mouse event escape sequence should enqueue into active TTY");

        code_t code = KEY_NONE;
        bool got = false;
        for (size_t attempt = 0; attempt < 32; ++attempt) {
            if (!tty_read_timeout(env->tty, 0, &code)) {
                break;
            }
            if (code == KEY_EVENT_RESIZE) {
                continue;
            }
            got = true;
            break;
        }

        EXPECT_TRUE(got, "mouse decode test should yield the expected number of key events");
        EXPECT_TRUE(code == cases[i].expected_code, "decoded mouse key code mismatch");

        tty_mouse_event_t mouse_event;
        EXPECT_TRUE(tty_get_last_mouse_event(env->tty, &mouse_event),
                    "mouse key events should capture metadata");
        EXPECT_TRUE(mouse_event.action == cases[i].expected_action,
                    "captured mouse action should match the decoded key event");
        EXPECT_TRUE(mouse_event.column == cases[i].expected_column &&
                        mouse_event.row == cases[i].expected_row,
                    "mouse metadata should preserve reported terminal coordinates");
        EXPECT_TRUE(mouse_event.modifiers == cases[i].expected_modifiers,
                    "mouse metadata should preserve modifier masks");
    }

    return true;
}

static bool test_tty_focus_event_decode(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t seq_focus_in[] = {'\x1B', '[', 'I'};
    static const uint8_t seq_focus_out[] = {'\x1B', '[', 'O'};

    static const struct {
        const uint8_t* raw;
        size_t raw_len;
        code_t expected;
        const char* label;
    } cases[] = {
        {seq_focus_in, sizeof(seq_focus_in), KEY_EVENT_FOCUS_IN,
         "focus-in escape sequence should decode into focus-in event"},
        {seq_focus_out, sizeof(seq_focus_out), KEY_EVENT_FOCUS_OUT,
         "focus-out escape sequence should decode into focus-out event"},
    };

    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i) {
        EXPECT_TRUE(ic_push_raw_input(cases[i].raw, cases[i].raw_len),
                    "focus event escape sequence should enqueue into active TTY");

        code_t code = KEY_NONE;
        bool got = false;
        for (size_t attempt = 0; attempt < 32; ++attempt) {
            if (!tty_read_timeout(env->tty, 0, &code)) {
                break;
            }
            if (code == KEY_EVENT_RESIZE) {
                continue;
            }
            got = true;
            break;
        }

        EXPECT_TRUE(got, "focus decode test should produce one key event");
        EXPECT_TRUE(code == cases[i].expected, cases[i].label);
    }

    return true;
}

static bool test_tty_kitty_ctrl_sequence_decode_regression(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    static const uint8_t seq_ctrl_a_caps_lock[] = {'\x1B', '[', '9', '7', ';', '6', '9', 'u'};
    static const uint8_t seq_ctrl_enter_caps_lock[] = {'\x1B', '[', '1', '3', ';', '6', '9', 'u'};

    static const struct {
        const uint8_t* raw;
        size_t raw_len;
        code_t expected;
        const char* label;
    } cases[] = {
        {seq_ctrl_a_caps_lock, sizeof(seq_ctrl_a_caps_lock), KEY_CTRL_A,
         "kitty ctrl+a sequence should decode to ctrl-a"},
        {seq_ctrl_enter_caps_lock, sizeof(seq_ctrl_enter_caps_lock), KEY_LINEFEED,
         "kitty ctrl+enter sequence should decode to linefeed"},
    };

    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i) {
        EXPECT_TRUE(ic_push_raw_input(cases[i].raw, cases[i].raw_len),
                    "kitty ctrl sequence should enqueue into active TTY");

        code_t code = KEY_NONE;
        bool got = false;
        for (size_t attempt = 0; attempt < 32; ++attempt) {
            if (!tty_read_timeout(env->tty, 0, &code)) {
                break;
            }
            if (code == KEY_EVENT_RESIZE) {
                continue;
            }
            got = true;
            break;
        }

        EXPECT_TRUE(got, "kitty ctrl sequence should yield a decoded key event");
        EXPECT_TRUE(code == cases[i].expected, cases[i].label);
    }

    return true;
}

static bool test_key_spec_ctrl_space_variants(void) {
    ic_keycode_t key = IC_KEY_NONE;
    EXPECT_TRUE(ic_parse_key_spec("ctrl+space", &key), "ctrl+space key spec should parse");
    EXPECT_TRUE(key == IC_KEY_CTRL_SPACE,
                "ctrl+space should map to dedicated ctrl-space keycode constant");

    EXPECT_TRUE(ic_parse_key_spec("ctrl+@", &key), "ctrl+@ key spec should parse");
    EXPECT_TRUE(key == IC_KEY_CTRL_SPACE, "ctrl+@ should map to the same keycode as ctrl+space");

    char formatted[64];
    EXPECT_TRUE(ic_format_key_spec(IC_KEY_CTRL_SPACE, formatted, sizeof(formatted)),
                "formatting ctrl-space keycode should succeed");
    EXPECT_STREQ(formatted, "ctrl+space",
                 "ctrl-space keycode should format to canonical ctrl+space name");

    EXPECT_TRUE(ic_parse_key_spec("ctrl+tab", &key), "ctrl+tab key spec should parse");
    EXPECT_TRUE(ic_format_key_spec(key, formatted, sizeof(formatted)),
                "formatting ctrl+tab keycode should succeed");
    EXPECT_STREQ(formatted, "ctrl+tab",
                 "ctrl+tab should preserve explicit named key in formatted output");

    return true;
}

static bool test_initial_input_env_lifecycle(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_env_clear_initial_input(env);
    EXPECT_TRUE(env->initial_input == NULL,
                "initial input should start cleared for lifecycle test");

    char mutable_seed[] = "echo seeded";
    ic_env_set_initial_input(env, mutable_seed, 4, true);
    EXPECT_TRUE(env->initial_input != NULL, "setting initial input should allocate env storage");
    EXPECT_STREQ(env->initial_input, "echo seeded",
                 "initial input setter should preserve original text");
    EXPECT_TRUE(env->initial_cursor_pos == 4 && env->initial_cursor_pos_set,
                "initial input setter should retain an explicit cursor");

    mutable_seed[0] = 'X';
    EXPECT_STREQ(env->initial_input, "echo seeded",
                 "initial input should be copied, not aliased to caller memory");

    ic_env_set_initial_input(env, "printf seeded", 0, false);
    EXPECT_STREQ(env->initial_input, "printf seeded",
                 "setting initial input again should replace previous stored value");
    EXPECT_TRUE(env->initial_cursor_pos == 0 && !env->initial_cursor_pos_set,
                "replacing initial input should replace its cursor metadata");

    ic_env_clear_initial_input(env);
    EXPECT_TRUE(env->initial_input == NULL, "clearing initial input should reset pointer to NULL");
    EXPECT_TRUE(env->initial_cursor_pos == 0 && !env->initial_cursor_pos_set,
                "clearing initial input should reset cursor metadata");

    return true;
}

static bool test_unicode_display_width_control_codepoints(void) {
    static const uint8_t control_mix[] = {'A', 0x01, 0x7F, 0x09, 'B'};
    size_t ansi_chars = 0;
    size_t visible_chars = 0;
    size_t display_width = unicode_calculate_display_width(
        (const char*)control_mix, sizeof(control_mix), &ansi_chars, &visible_chars);

    EXPECT_TRUE(display_width == 2,
                "control bytes should have zero width while printable bytes keep normal width");
    EXPECT_TRUE(ansi_chars == 0, "plain control bytes should not be counted as ANSI escape bytes");
    EXPECT_TRUE(visible_chars == 2,
                "visible char count should exclude zero-width control codepoints");

    size_t utf8_width = unicode_calculate_utf8_width((const char*)control_mix, sizeof(control_mix));
    EXPECT_TRUE(utf8_width == 2,
                "utf8 width helper should also treat C0/C1 controls as zero-width characters");

    return true;
}

static bool test_unicode_display_width_osc_sequence_ignored(void) {
    static const uint8_t osc_title[] = {'\x1B', ']', '0', ';',    't', 'i',
                                        't',    'l', 'e', '\x07', 'X'};

    size_t ansi_chars = 0;
    size_t visible_chars = 0;
    size_t width = unicode_calculate_display_width((const char*)osc_title, sizeof(osc_title),
                                                   &ansi_chars, &visible_chars);
    EXPECT_TRUE(width == 1, "OSC escape sequence bytes should not contribute to display width");
    EXPECT_TRUE(visible_chars == 1,
                "only printable payload after OSC sequence should count as visible text");
    EXPECT_TRUE(ansi_chars == 10, "ANSI byte counter should include complete OSC sequence bytes");

    return true;
}

static bool test_history_search_direction_and_position(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_search_behavior.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "echo alpha"), "first history entry should be stored");
    EXPECT_TRUE(history_push(history, "printf beta"), "second history entry should be stored");
    EXPECT_TRUE(history_push(history, "grep gamma"), "third history entry should be stored");

    ssize_t idx = -1;
    ssize_t pos = -1;
    EXPECT_TRUE(history_search(history, 0, "beta", true, &idx, &pos),
                "backward history search should find substring in older entries");
    EXPECT_TRUE(idx == 1, "backward search should report history index relative to newest entry");
    EXPECT_TRUE(pos == 7,
                "search position should point to substring offset inside matched command");

    idx = -1;
    pos = -1;
    EXPECT_TRUE(history_search(history, 2, "echo", false, &idx, &pos),
                "forward history search should find substring in older-to-newer direction");
    EXPECT_TRUE(idx == 2, "forward search should preserve found history offset in result index");
    EXPECT_TRUE(pos == 0, "search position should be zero for command-prefix match");

    idx = -1;
    EXPECT_FALSE(history_search(history, 0, "missing", true, &idx, NULL),
                 "search should fail for substrings absent from all history entries");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_fuzzy_metadata_filtering(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_metadata_filter.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    const ic_history_metadata_t build_meta[] = {
        {"result", "ok"},
        {"tag", "ci"},
    };
    const ic_history_metadata_t run_meta[] = {
        {"result", "fail"},
        {"tag", "ci"},
        {"exit_code", "2"},
    };
    const ic_history_metadata_t deploy_meta[] = {
        {"result", "fail"},
        {"tag", "release"},
    };

    EXPECT_TRUE(history_push_with_metadata(history, "build project", build_meta,
                                           sizeof(build_meta) / sizeof(build_meta[0])),
                "history should accept metadata for successful entries");
    EXPECT_TRUE(history_push_with_metadata(history, "run tests", run_meta,
                                           sizeof(run_meta) / sizeof(run_meta[0])),
                "history should accept metadata for failed entries");
    EXPECT_TRUE(history_push_with_metadata(history, "deploy", deploy_meta,
                                           sizeof(deploy_meta) / sizeof(deploy_meta[0])),
                "history should accept repeated metadata values");

    history_match_t matches[8];
    ssize_t match_count = 0;
    bool metadata_filter_applied = false;

    EXPECT_TRUE(history_fuzzy_search(history, "result::fail", matches, 8, &match_count,
                                     &metadata_filter_applied),
                "metadata-only query should return entries matching requested key/value");
    EXPECT_TRUE(metadata_filter_applied,
                "metadata query should report metadata filter application");
    EXPECT_TRUE(match_count == 2, "metadata-only query should return both matching entries");

    for (ssize_t i = 0; i < match_count; ++i) {
        const char* cmd = history_get(history, matches[i].hidx);
        EXPECT_TRUE(cmd != NULL, "matched history entry should be retrievable by index");
        EXPECT_FALSE(strcmp(cmd, "build project") == 0,
                     "metadata filtering should exclude non-matching success entries");
    }

    match_count = 0;
    metadata_filter_applied = false;
    EXPECT_TRUE(history_fuzzy_search(history, "run tag::ci", matches, 8, &match_count,
                                     &metadata_filter_applied),
                "combined text and metadata query should keep text fuzzy matching active");
    EXPECT_TRUE(metadata_filter_applied,
                "combined query should still apply and report metadata filtering");
    EXPECT_TRUE(match_count >= 1,
                "combined text and metadata query should produce at least one filtered match");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, true),
                "snapshot should load entries with metadata intact");
    const history_entry_t* newest = history_snapshot_get(&snap, 0);
    EXPECT_TRUE(newest != NULL, "snapshot should include newest entry");
    EXPECT_TRUE(history_entry_get_metadata(newest, "timestamp") == NULL,
                "user metadata should suppress the default timestamp");
    EXPECT_TRUE(history_entry_get_metadata(newest, "frequency") == NULL,
                "user metadata should suppress the default frequency");
    history_snapshot_free(history, &snap);

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_disabled_mode_rejects_push(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_disabled.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 0);

    EXPECT_FALSE(history_push(history, "echo never"),
                 "disabled history should reject pushes regardless of entry content");
    EXPECT_TRUE(history_count(history) == 0, "disabled history should report zero entries");

    history_match_t matches[2];
    ssize_t match_count = 77;
    EXPECT_FALSE(history_fuzzy_search(history, "echo", matches, 2, &match_count, NULL),
                 "disabled history should not produce fuzzy-search results");
    EXPECT_TRUE(match_count == 0,
                "disabled history fuzzy search should reset output match count to zero");

    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_key_spec_separator_and_invalid_forms(void) {
    ic_keycode_t key = IC_KEY_NONE;

    EXPECT_TRUE(ic_parse_key_spec("ctrl-x", &key),
                "dash-separated modifier specs should parse equivalently to plus-separated specs");
    EXPECT_TRUE(key == IC_KEY_CTRL_X, "ctrl-x should parse to ctrl-X keycode");

    EXPECT_TRUE(ic_parse_key_spec("  alt + left  ", &key),
                "key specs with surrounding whitespace should parse successfully");
    EXPECT_TRUE(key == IC_KEY_WITH_ALT(IC_KEY_LEFT),
                "whitespace-normalized key spec should preserve modifier semantics");

    EXPECT_FALSE(ic_parse_key_spec("ctrl+a+b", &key),
                 "multiple base-key tokens in one key spec should be rejected");
    EXPECT_FALSE(ic_parse_key_spec("", &key), "empty key spec should be rejected");
    EXPECT_FALSE(ic_parse_key_spec(NULL, &key), "NULL key spec should be rejected");

    return true;
}

static bool test_key_binding_named_invalid_inputs(void) {
    EXPECT_FALSE(ic_bind_key_named("ctrl+x", "not-a-real-action"),
                 "bind_key_named should reject unknown action names");
    EXPECT_FALSE(ic_bind_key_named("not-a-real-key", "undo"),
                 "bind_key_named should reject unknown key specification names");
    EXPECT_FALSE(ic_bind_key_named(NULL, "undo"), "bind_key_named should reject NULL key specs");
    EXPECT_FALSE(ic_bind_key_named("ctrl+x", NULL),
                 "bind_key_named should reject NULL action names");

    EXPECT_TRUE(ic_key_binding_profile_default_specs(IC_KEY_ACTION_NONE) == NULL,
                "default spec lookup should reject non-bindable NONE action");
    EXPECT_TRUE(ic_key_binding_profile_default_specs(IC_KEY_ACTION__MAX) == NULL,
                "default spec lookup should reject out-of-range action sentinel");

    return true;
}

static bool test_tty_code_pushback_order_and_capacity_guard(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->tty == NULL) {
        return true;
    }

    code_t drained = KEY_NONE;
    while (tty_read_timeout(env->tty, 0, &drained)) {
    }

    const ic_keycode_t sequence[] = {KEY_CTRL_A, KEY_CTRL_B, KEY_CTRL_C};
    EXPECT_TRUE(ic_push_key_sequence(sequence, 3),
                "key sequence push should succeed with active TTY");

    code_t popped = KEY_NONE;
    EXPECT_TRUE(tty_read_timeout(env->tty, 0, &popped),
                "first queued key should be readable without waiting for terminal input");
    EXPECT_TRUE(popped == KEY_CTRL_A,
                "queued key sequence should preserve logical order for first key");

    EXPECT_TRUE(tty_read_timeout(env->tty, 0, &popped),
                "second queued key should be readable without waiting for terminal input");
    EXPECT_TRUE(popped == KEY_CTRL_B,
                "queued key sequence should preserve logical order for second key");

    EXPECT_TRUE(tty_read_timeout(env->tty, 0, &popped),
                "third queued key should be readable without waiting for terminal input");
    EXPECT_TRUE(popped == KEY_CTRL_C,
                "queued key sequence should preserve logical order for third key");

    return true;
}

static bool test_term_manual_visibility_override_api(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL) {
        return false;
    }

    term_reset_line_state(env->term);
    ic_term_mark_line_visible(true);
    EXPECT_TRUE(term_line_has_visible_content(env->term),
                "public manual visibility marker should force visible-line state true");

    ic_term_mark_line_visible(false);
    EXPECT_FALSE(term_line_has_visible_content(env->term),
                 "public manual visibility marker should force visible-line state false");

    return true;
}

static bool test_prompt_line_replacement_gate_matrix(void) {
    ic_prompt_line_replacement_state_t state = {
        .replace_prompt_line_with_line_number = true,
        .prompt_has_prefix_lines = true,
        .prompt_begins_with_newline = false,
        .line_numbers_enabled = true,
        .input_has_content = true,
    };

    EXPECT_TRUE(ic_prompt_line_replacement_should_activate(&state),
                "all required predicate gates enabled should activate prompt replacement");

    state.replace_prompt_line_with_line_number = false;
    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(&state),
                 "replacement should not activate when feature flag is disabled");

    state.replace_prompt_line_with_line_number = true;
    state.prompt_has_prefix_lines = false;
    state.prompt_begins_with_newline = false;
    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(&state),
                 "replacement should not activate without prefix lines or leading newline");

    state.prompt_begins_with_newline = true;
    EXPECT_TRUE(ic_prompt_line_replacement_should_activate(&state),
                "leading newline alone should satisfy prompt structure requirement");

    state.line_numbers_enabled = false;
    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(&state),
                 "replacement should not activate when line numbers are disabled");

    state.line_numbers_enabled = true;
    state.input_has_content = false;
    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(&state),
                 "replacement should keep prompt visible while input buffer is empty");

    EXPECT_FALSE(ic_prompt_line_replacement_should_activate(NULL),
                 "replacement predicate should reject NULL state pointers");

    return true;
}

static bool test_bbcode_default_styles_are_registered(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL || env->term == NULL || env->bbcode == NULL) {
        return false;
    }

    const attr_t prompt_attr = bbcode_style(env->bbcode, "ic-prompt");
    const attr_t error_attr = bbcode_style(env->bbcode, "ic-error");
    EXPECT_FALSE(attr_is_none(prompt_attr),
                 "default prompt style should resolve to a concrete attribute mapping");
    EXPECT_FALSE(attr_is_none(error_attr),
                 "default error style should resolve to a concrete attribute mapping");
    EXPECT_FALSE(attr_is_eq(prompt_attr, error_attr),
                 "prompt and error styles should remain visually distinct for UI rendering");

    return true;
}

static bool test_history_update_and_remove_last_flow(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_update_flow.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "echo first"), "first command should be pushed to history");
    EXPECT_TRUE(history_push(history, "echo second"), "second command should be pushed to history");

    EXPECT_TRUE(history_update(history, "  printf replaced  "),
                "history_update should replace and normalize the latest entry");
    const char* latest = history_get(history, 0);
    EXPECT_STREQ(
        latest, "  printf replaced  ",
        "history_update should overwrite the most recent history command text as provided");

    history_remove_last(history);
    EXPECT_TRUE(history_count(history) == 1,
                "history_remove_last should remove exactly one most recent entry");
    EXPECT_STREQ(history_get(history, 0), "echo first",
                 "after removing latest entry, previous command should become newest");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_snapshot_bounds_and_order(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_snapshot_order.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "cmd one"), "first snapshot entry should persist");
    EXPECT_TRUE(history_push(history, "cmd two"), "second snapshot entry should persist");
    EXPECT_TRUE(history_push(history, "cmd three"), "third snapshot entry should persist");

    history_snapshot_t snap = {0};
    EXPECT_TRUE(history_snapshot_load(history, &snap, true), "snapshot load should succeed");
    EXPECT_TRUE(history_snapshot_count(&snap) == 3,
                "snapshot count helper should report number of collected entries");

    EXPECT_TRUE(history_snapshot_get(&snap, -1) == NULL,
                "snapshot getter should reject negative indexes");
    EXPECT_TRUE(history_snapshot_get(&snap, 3) == NULL,
                "snapshot getter should reject indexes beyond last entry");

    const history_entry_t* newest = history_snapshot_get(&snap, 0);
    const history_entry_t* oldest = history_snapshot_get(&snap, 2);
    EXPECT_TRUE(newest != NULL && oldest != NULL,
                "snapshot getter should return valid pointers for in-range indexes");
    EXPECT_STREQ(newest->command, "cmd three",
                 "snapshot index zero should map to most recent command");
    EXPECT_STREQ(oldest->command, "cmd one",
                 "highest valid snapshot index should map to oldest command");

    history_snapshot_free(history, &snap);
    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_search_prefix_empty_query_behavior(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_prefix_empty.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "alpha"), "prefix-search fixture first entry should persist");
    EXPECT_TRUE(history_push(history, "beta"), "prefix-search fixture second entry should persist");

    ssize_t idx = -1;
    EXPECT_TRUE(history_search_prefix(history, 0, "", true, &idx),
                "empty prefix backward search should accept in-range starting index");
    EXPECT_TRUE(idx == 0, "empty prefix backward search should preserve provided starting index");

    idx = -1;
    EXPECT_TRUE(history_search_prefix(history, 1, "", false, &idx),
                "empty prefix forward search should accept non-negative starting index");
    EXPECT_TRUE(idx == 1, "empty prefix forward search should preserve provided starting index");

    idx = -1;
    EXPECT_FALSE(history_search_prefix(history, -1, "", false, &idx),
                 "empty prefix forward search should reject negative starting index");

    idx = -1;
    EXPECT_TRUE(history_search_prefix(history, 0, "be", true, &idx),
                "non-empty prefix search should still work after empty-prefix checks");
    EXPECT_TRUE(idx == 0, "newest entry should be matched first when searching backward");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_unicode_display_width_malformed_csi_sequences(void) {
    static const uint8_t esc_only[] = {'\x1B'};
    static const uint8_t csi_incomplete[] = {'\x1B', '[', '3', '1'};

    size_t ansi_chars = 0;
    size_t visible_chars = 0;
    size_t width = unicode_calculate_display_width((const char*)esc_only, sizeof(esc_only),
                                                   &ansi_chars, &visible_chars);
    EXPECT_TRUE(width == 0, "standalone ESC byte should be treated as zero-width control input");
    EXPECT_TRUE(ansi_chars == 0, "standalone ESC byte should not be counted as ANSI sequence");
    EXPECT_TRUE(visible_chars == 0,
                "standalone ESC byte should not contribute visible character width");

    ansi_chars = 0;
    visible_chars = 0;
    width = unicode_calculate_display_width((const char*)csi_incomplete, sizeof(csi_incomplete),
                                            &ansi_chars, &visible_chars);
    EXPECT_TRUE(width == 0,
                "incomplete CSI byte sequences should be consumed as non-visible ANSI bytes");
    EXPECT_TRUE(ansi_chars == 4,
                "ANSI byte accounting should include all bytes inside incomplete CSI sequence");
    EXPECT_TRUE(visible_chars == 0,
                "incomplete CSI sequence should not produce visible character width");

    return true;
}

static bool test_unicode_display_width_invalid_utf8_fallback(void) {
    static const uint8_t invalid_utf8[] = {0xFF, 0xFE, 'A'};

    size_t ansi_chars = 0;
    size_t visible_chars = 0;
    size_t width = unicode_calculate_display_width((const char*)invalid_utf8, sizeof(invalid_utf8),
                                                   &ansi_chars, &visible_chars);
    EXPECT_TRUE(width == 3,
                "invalid UTF-8 bytes should fall back to single-width visible placeholders");
    EXPECT_TRUE(visible_chars == 3,
                "invalid UTF-8 bytes should still increment visible character count");
    EXPECT_TRUE(ansi_chars == 0, "invalid UTF-8 bytes should not be misclassified as ANSI escapes");

    size_t utf8_width =
        unicode_calculate_utf8_width((const char*)invalid_utf8, sizeof(invalid_utf8));
    EXPECT_TRUE(utf8_width == 3,
                "UTF-8 width helper should mirror fallback width behavior for invalid bytes");

    return true;
}

static bool test_history_max_entries_pruning(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_max_entries.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 2);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "cmd one"), "first history entry should be accepted");
    EXPECT_TRUE(history_push(history, "cmd two"), "second history entry should be accepted");
    EXPECT_TRUE(history_push(history, "cmd three"),
                "third history entry should be accepted and trigger pruning");

    EXPECT_TRUE(history_count(history) == 2,
                "history should prune oldest entries to configured max size");
    EXPECT_STREQ(history_get(history, 0), "cmd three",
                 "newest command should remain available after pruning");
    EXPECT_STREQ(history_get(history, 1), "cmd two",
                 "second newest command should remain after pruning oldest entry");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_snapshot_load_dedup_mode(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_snapshot_dedup.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 16);
    history_clear(history);

    (void)history_enable_duplicates(history, true);
    EXPECT_TRUE(history_push(history, "dup"), "first duplicate entry should be stored");
    EXPECT_TRUE(history_push(history, "dup"), "second duplicate entry should be stored");
    EXPECT_TRUE(history_push(history, "unique"), "unique trailing entry should be stored");
    (void)history_enable_duplicates(history, false);

    history_snapshot_t deduped = {0};
    EXPECT_TRUE(history_snapshot_load(history, &deduped, true),
                "snapshot load with dedup=true should succeed");
    EXPECT_TRUE(deduped.count == 2, "dedup snapshot should collapse duplicate command instances");

    bool saw_dup = false;
    bool saw_unique = false;
    for (ssize_t i = 0; i < deduped.count; ++i) {
        const history_entry_t* entry = history_snapshot_get(&deduped, i);
        if (entry == NULL || entry->command == NULL) {
            continue;
        }
        if (strcmp(entry->command, "dup") == 0) {
            saw_dup = true;
        }
        if (strcmp(entry->command, "unique") == 0) {
            saw_unique = true;
        }
    }
    EXPECT_TRUE(saw_dup && saw_unique,
                "dedup snapshot should retain one duplicate plus unique entries");

    history_snapshot_free(history, &deduped);
    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_remove_last_on_empty_safe(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_remove_empty.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 8);
    history_clear(history);

    history_remove_last(history);
    EXPECT_TRUE(history_count(history) == 0,
                "removing last entry from empty history should stay a no-op");

    EXPECT_TRUE(history_push(history, "solo"), "single history entry should be storable");
    history_remove_last(history);
    EXPECT_TRUE(history_count(history) == 0,
                "removing last entry from singleton history should leave it empty");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_history_fuzzy_max_matches_cap(void) {
    alloc_t* mem = test_allocator();
    if (mem == NULL) {
        return false;
    }

    history_t* history = history_new(mem);
    if (history == NULL) {
        return false;
    }

    const char* history_path = "./isocline_history_fuzzy_cap.log";
    (void)remove(history_path);
    history_load_from(history, history_path, 32);
    history_clear(history);

    EXPECT_TRUE(history_push(history, "build one"), "fuzzy fixture entry should be stored");
    EXPECT_TRUE(history_push(history, "build two"), "fuzzy fixture entry should be stored");
    EXPECT_TRUE(history_push(history, "build three"), "fuzzy fixture entry should be stored");
    EXPECT_TRUE(history_push(history, "build four"), "fuzzy fixture entry should be stored");

    history_match_t matches[2];
    ssize_t match_count = 0;
    EXPECT_TRUE(history_fuzzy_search(history, "build", matches, 2, &match_count, NULL),
                "fuzzy search should produce matches for common query token");
    EXPECT_TRUE(match_count == 2,
                "fuzzy search should cap returned matches to requested max_matches value");
    EXPECT_TRUE(matches[0].score >= matches[1].score,
                "fuzzy search results should be sorted in descending score order");

    history_clear(history);
    history_free(history);
    (void)remove(history_path);
    return true;
}

static bool test_key_spec_alias_and_f24_roundtrip(void) {
    ic_keycode_t key = IC_KEY_NONE;

    EXPECT_TRUE(ic_parse_key_spec("meta+x", &key), "meta alias should parse as alt modifier");
    EXPECT_TRUE(key == IC_KEY_WITH_ALT(ic_key_char('x')),
                "meta alias should produce alt-modified character keycode");

    EXPECT_TRUE(ic_parse_key_spec("option+x", &key), "option alias should parse as alt modifier");
    EXPECT_TRUE(key == IC_KEY_WITH_ALT(ic_key_char('x')),
                "option alias should map to same keycode as meta alias");

    EXPECT_TRUE(ic_parse_key_spec("return", &key), "return alias should parse as enter key");
    EXPECT_TRUE(key == IC_KEY_ENTER, "return alias should map to enter key constant");

    EXPECT_TRUE(ic_parse_key_spec("newline", &key), "newline alias should parse as linefeed key");
    EXPECT_TRUE(key == IC_KEY_LINEFEED, "newline alias should map to linefeed constant");

    EXPECT_TRUE(ic_parse_key_spec("f24", &key),
                "f24 key spec should parse for extended function keys");
    char formatted[64];
    EXPECT_TRUE(ic_format_key_spec(key, formatted, sizeof(formatted)),
                "formatted output for f24 keycode should succeed");
    EXPECT_STREQ(formatted, "f24", "f24 keycode should format to canonical f24 token");

    return true;
}

static bool test_key_queue_api_noop_inputs(void) {
    EXPECT_TRUE(ic_push_key_sequence(NULL, 0),
                "NULL key sequence with zero count should be accepted as a no-op");

    const ic_keycode_t dummy = KEY_ENTER;
    EXPECT_TRUE(ic_push_key_sequence(&dummy, 0),
                "non-NULL key pointer with zero count should be accepted as no-op");

    return true;
}

static bool test_command_palette_entry_registration_and_listing(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_clear_command_palette_entries();
    EXPECT_TRUE(ic_list_command_palette_entries(NULL, 0) == 0,
                "clearing command palette entries should leave registry empty");

    EXPECT_FALSE(ic_set_command_palette_entries(NULL, 1),
                 "setting command palette entries should reject NULL array with non-zero count");

    const ic_command_palette_entry_t invalid_entry[] = {
        {"invalid", "", "", ""},
    };
    EXPECT_FALSE(ic_set_command_palette_entries(invalid_entry, 1),
                 "command palette entries should reject empty display names");

    const ic_command_palette_entry_t entries[] = {
        {"snippet-for", "Snippet: for loop", "insert a for-loop skeleton",
         "snippet for loop template"},
        {NULL, "Snippet: if guard", "insert an if statement guard", "snippet if guard template"},
    };

    EXPECT_TRUE(ic_set_command_palette_entries(entries, 2),
                "setting command palette entries should accept valid metadata rows");
    EXPECT_TRUE(env->command_palette_entry_count == 2,
                "environment should track registered command palette entry count");

    EXPECT_TRUE(ic_list_command_palette_entries(NULL, 0) == 2,
                "listing command palette entries should report full count when buffer is NULL");

    ic_command_palette_entry_t listed[4];
    size_t listed_count = ic_list_command_palette_entries(listed, 4);
    EXPECT_TRUE(
        listed_count == 2,
        "listing command palette entries should return all registered entries when buffer is "
        "large enough");
    EXPECT_STREQ(listed[0].id, "snippet-for",
                 "listed command palette entry should preserve explicit id field");
    EXPECT_STREQ(listed[0].name, "Snippet: for loop",
                 "listed command palette entry should preserve display name");
    EXPECT_STREQ(listed[1].id, "Snippet: if guard",
                 "entry id should default to display name when id is omitted");

    const ic_command_palette_entry_t replacement[] = {
        {"snippet-while", "Snippet: while loop", "insert a while-loop skeleton",
         "snippet while loop template"},
    };
    EXPECT_TRUE(ic_set_command_palette_entries(replacement, 1),
                "setting command palette entries should replace prior registrations");
    EXPECT_TRUE(ic_list_command_palette_entries(NULL, 0) == 1,
                "replacing command palette entries should shrink registry to new count");

    ic_clear_command_palette_entries();
    EXPECT_TRUE(ic_list_command_palette_entries(NULL, 0) == 0,
                "command palette entry clear API should remove all registered rows");
    return true;
}

static bool test_command_palette_handler_registration(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_set_command_palette_entry_handler(NULL, NULL);
    EXPECT_TRUE(env->command_palette_handler == NULL,
                "clearing command palette handler should reset callback pointer");
    EXPECT_TRUE(env->command_palette_handler_arg == NULL,
                "clearing command palette handler should reset callback argument");

    ic_set_command_palette_entry_handler(stub_command_palette_handler, (void*)0xFACE);
    EXPECT_TRUE(env->command_palette_handler == stub_command_palette_handler,
                "command palette handler setter should store callback pointer verbatim");
    EXPECT_TRUE(env->command_palette_handler_arg == (void*)0xFACE,
                "command palette handler setter should store callback user argument");

    g_command_palette_handler_called = false;
    g_command_palette_handler_last_id = NULL;
    g_command_palette_handler_last_arg = NULL;

    const ic_command_palette_entry_t sample = {"snippet-for", "Snippet: for loop", "", ""};
    EXPECT_TRUE(env->command_palette_handler(&sample, env->command_palette_handler_arg),
                "stored command palette handler callback should remain invokable");
    EXPECT_TRUE(g_command_palette_handler_called,
                "command palette handler test stub should record invocation");
    EXPECT_STREQ(g_command_palette_handler_last_id, "snippet-for",
                 "command palette handler should receive selected entry metadata");
    EXPECT_TRUE(g_command_palette_handler_last_arg == (void*)0xFACE,
                "command palette handler should receive configured callback argument");

    ic_set_command_palette_entry_handler(NULL, NULL);
    return true;
}

static bool test_custom_menu_rejects_calls_without_active_editor(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    env->current_editor = NULL;
    const ic_menu_item_t items[] = {
        {"Status", "show repository status", "git working tree"},
    };
    size_t selected = 42;
    EXPECT_FALSE(ic_show_menu("actions: ", items, 1, &selected),
                 "custom menu should require an active readline editor");
    EXPECT_TRUE(selected == 42,
                "failed custom menu calls should leave the selected index unchanged");
    EXPECT_FALSE(ic_show_menu("actions: ", NULL, 1, &selected),
                 "custom menu should reject a NULL item array");
    EXPECT_FALSE(ic_show_menu("actions: ", items, 0, &selected),
                 "custom menu should reject an empty item array");
    ic_menu_accept_t accept = IC_MENU_ACCEPT_SUBMIT;
    EXPECT_FALSE(ic_show_menu_ex("actions: ", items, 1, &selected, &accept),
                 "extended custom menu should require an active readline editor");
    EXPECT_TRUE(accept == IC_MENU_ACCEPT_NONE,
                "failed extended custom menu calls should clear the accept result");
    EXPECT_FALSE(ic_current_loop_advance("replacement"),
                 "advancing the editor should require an active readline operation");
    EXPECT_FALSE(ic_current_loop_advance_with_prompt("replacement", "final> ", NULL),
                 "advancing with a replacement prompt should require an active editor");
    return true;
}

static bool test_status_message_callback_registration(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    ic_set_status_message_callback(NULL, NULL);
    EXPECT_TRUE(env->status_message_callback == NULL,
                "clearing status callback should reset callback pointer");
    EXPECT_TRUE(env->status_message_arg == NULL,
                "clearing status callback should reset callback argument");

    ic_set_status_message_callback(stub_status_message, (void*)0xC0DE);
    EXPECT_TRUE(env->status_message_callback == stub_status_message,
                "status callback setter should store callback pointer verbatim");
    EXPECT_TRUE(env->status_message_arg == (void*)0xC0DE,
                "status callback setter should store callback user argument");

    ic_set_status_message_callback(NULL, NULL);
    return true;
}

static bool test_term_color_bits_and_toggle_roundtrip(void) {
    ic_env_t* env = ensure_env();
    if (env == NULL) {
        return false;
    }

    int bits = ic_term_get_color_bits();
    EXPECT_TRUE(bits == 1 || bits == 3 || bits == 4 || bits == 8 || bits == 24,
                "reported terminal color depth should be one of supported palette sizes");

    if (env->term != NULL) {
        bool previous = ic_enable_color(false);
        bool restored = ic_enable_color(previous);
        EXPECT_FALSE(restored,
                     "re-enabling color should report it was disabled by the previous toggle");
    }

    return true;
}

static bool test_history_entry_decoder(void) {
    const char* encoded = "a\\n\\t\\r\\\\\\x23\\xAf ";
    const char expected[] = {'a', '\n', '\t', '\\', '#', (char)0xAF, ' ', '\0'};
    char decoded[64];
    size_t length = 99;
    EXPECT_TRUE(
        ic_history_decode_entry(encoded, strlen(encoded), decoded, sizeof(decoded), &length),
        "history escapes should decode through the public API");
    EXPECT_TRUE(length == sizeof(expected) - 1 && memcmp(decoded, expected, sizeof(expected)) == 0,
                "decoder should preserve whitespace and non-ASCII bytes");
    const char* invalid[] = {"\\", "\\x", "\\x1", "\\xZZ", "\\q"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        length = 99;
        EXPECT_FALSE(ic_history_decode_entry(invalid[i], strlen(invalid[i]), decoded,
                                             sizeof(decoded), &length),
                     "malformed escapes should be rejected");
        EXPECT_TRUE(length == 0, "failed decode should reset the reported length");
    }
    char inplace[] = "a\\n\\x62";
    EXPECT_TRUE(
        ic_history_decode_entry(inplace, strlen(inplace), inplace, sizeof(inplace), &length),
        "history decoding should support an overlapping input/output buffer");
    EXPECT_STREQ(inplace, "a\nb", "in-place history decoding should retain the decoded content");
    EXPECT_FALSE(ic_history_decode_entry("abc", 3, decoded, 3, &length),
                 "decoder should require space for the terminating zero");
    EXPECT_TRUE(ic_history_decode_entry("", 0, decoded, sizeof(decoded), &length),
                "empty entries should decode successfully");
    EXPECT_TRUE(length == 0 && decoded[0] == '\0', "empty entries should be terminated");
    EXPECT_TRUE(ic_history_decode_entry("\\x00x", 5, decoded, sizeof(decoded), &length),
                "decoder should accept an embedded zero byte");
    EXPECT_TRUE(length == 2 && decoded[0] == '\0' && decoded[1] == 'x',
                "embedded zero bytes should not truncate the decoded length");
    return true;
}

typedef bool (*test_fn_t)(void);

typedef struct test_case_s {
    const char* name;
    test_fn_t fn;
} test_case_t;

static const test_case_t kTests[] = {
    {"history_entry_decoder", test_history_entry_decoder},
    {"mouse_reporting_defaults", test_mouse_reporting_defaults},
    {"readline_disposition_name_mappings", test_readline_disposition_name_mappings},
    {"multiline_toggle", test_multiline_toggle},
    {"multiline_continuation_retention_toggle", test_multiline_continuation_retention_toggle},
    {"line_number_modes", test_line_number_modes},
    {"line_number_continuation_prompt_toggle", test_line_number_continuation_prompt_toggle},
    {"line_number_prompt_replacement_toggle", test_line_number_prompt_replacement_toggle},
    {"prompt_line_replacement_requires_content", test_prompt_line_replacement_requires_content},
    {"line_wrap_marker", test_line_wrap_marker},
    {"visible_whitespace_marker", test_visible_whitespace_marker},
    {"multiline_start_line_count_clamp", test_multiline_start_line_count_clamp},
    {"multiline_max_line_count_defaults_and_clamps",
     test_multiline_max_line_count_defaults_and_clamps},
    {"multiline_bottom_line_count_defaults_and_clamps",
     test_multiline_bottom_line_count_defaults_and_clamps},
    {"menu_max_line_count_defaults_and_clamps", test_menu_max_line_count_defaults_and_clamps},
    {"multiline_viewport_layout", test_multiline_viewport_layout},
    {"multiline_viewport_bottom_content_rows", test_multiline_viewport_bottom_content_rows},
    {"multiline_viewport_symmetric_scroll_margin", test_multiline_viewport_symmetric_scroll_margin},
    {"editline_buffer_api_without_editor", test_editline_buffer_api_without_editor},
    {"continuation_callback_registration", test_continuation_callback_registration},
    {"completion_generation_and_apply", test_completion_generation_and_apply},
    {"history_dedup_snapshot", test_history_dedup_snapshot},
    {"history_snapshot_search_consistency", test_history_snapshot_search_consistency},
    {"history_directory_scope", test_history_directory_scope},
    {"history_fuzzy_case_toggle", test_history_fuzzy_case_toggle},
    {"history_fuzzy_case_toggle_via_api", test_history_fuzzy_case_toggle_via_api},
    {"history_search_sort_api", test_history_search_sort_api},
    {"line_wrapping_calculations", test_line_wrapping_calculations},
    {"unicode_decode_utf8_valid_sequences", test_unicode_decode_utf8_valid_sequences},
    {"unicode_decode_utf8_invalid_sequences", test_unicode_decode_utf8_invalid_sequences},
    {"unicode_encode_utf8_roundtrip", test_unicode_encode_utf8_roundtrip},
    {"unicode_qutf8_raw_byte_roundtrip", test_unicode_qutf8_raw_byte_roundtrip},
    {"unicode_width_calculation_with_invalid_and_ansi",
     test_unicode_width_calculation_with_invalid_and_ansi},
    {"str_next_ofs_with_utf8_and_escape_sequences",
     test_str_next_ofs_with_utf8_and_escape_sequences},
    {"stringbuf_empty_non_utf8_result", test_stringbuf_empty_non_utf8_result},
    {"stringbuf_utf8_navigation_and_deletion", test_stringbuf_utf8_navigation_and_deletion},
    {"push_raw_input_preconditions", test_push_raw_input_preconditions},
    {"tty_character_pushback_capacity_guard", test_tty_character_pushback_capacity_guard},
    {"push_raw_input_null_pointer_rejected", test_push_raw_input_null_pointer_rejected},
    {"tty_capture_pending_raw_preconditions", test_tty_capture_pending_raw_preconditions},
    {"escape_skip_charset_sequence_length", test_escape_skip_charset_sequence_length},
    {"typeahead_toggle_lifecycle", test_typeahead_toggle_lifecycle},
    {"typeahead_clear_pending_state", test_typeahead_clear_pending_state},
    {"typeahead_capture_gate_registration_and_disabled_state",
     test_typeahead_capture_gate_registration_and_disabled_state},
    {"typeahead_filter_preserves_plain_text", test_typeahead_filter_preserves_plain_text},
    {"typeahead_filter_removes_csi_sequences", test_typeahead_filter_removes_csi_sequences},
    {"typeahead_filter_removes_osc_sequences", test_typeahead_filter_removes_osc_sequences},
    {"typeahead_filter_removes_charset_numeric_and_control_bytes",
     test_typeahead_filter_removes_charset_numeric_and_control_bytes},
    {"typeahead_filter_reuses_extended_escape_skipper",
     test_typeahead_filter_reuses_extended_escape_skipper},
    {"typeahead_normalize_backspace_and_delete", test_typeahead_normalize_backspace_and_delete},
    {"typeahead_normalize_backspace_deletes_full_utf8_codepoint",
     test_typeahead_normalize_backspace_deletes_full_utf8_codepoint},
    {"typeahead_normalize_ctrl_u_keeps_previous_lines",
     test_typeahead_normalize_ctrl_u_keeps_previous_lines},
    {"typeahead_normalize_ctrl_u_stops_at_carriage_return",
     test_typeahead_normalize_ctrl_u_stops_at_carriage_return},
    {"typeahead_normalize_ctrl_w_deletes_previous_word",
     test_typeahead_normalize_ctrl_w_deletes_previous_word},
    {"typeahead_normalize_ctrl_w_deletes_utf8_word",
     test_typeahead_normalize_ctrl_w_deletes_utf8_word},
    {"typeahead_normalize_ctrl_w_stops_at_carriage_return",
     test_typeahead_normalize_ctrl_w_stops_at_carriage_return},
    {"typeahead_ingest_rejects_disabled_null_and_empty_input",
     test_typeahead_ingest_rejects_disabled_null_and_empty_input},
    {"typeahead_ingest_plain_text_sets_pending_initial_input",
     test_typeahead_ingest_plain_text_sets_pending_initial_input},
    {"typeahead_ingest_appends_to_existing_pending_input",
     test_typeahead_ingest_appends_to_existing_pending_input},
    {"typeahead_ingest_filters_escape_sequences_before_pending_input",
     test_typeahead_ingest_filters_escape_sequences_before_pending_input},
    {"typeahead_ingest_preserves_carriage_return_as_submit",
     test_typeahead_ingest_preserves_carriage_return_as_submit},
    {"typeahead_ingest_preserves_ctrl_j_as_line_feed",
     test_typeahead_ingest_preserves_ctrl_j_as_line_feed},
    {"typeahead_ingest_keeps_last_submitted_line_with_trailing_return",
     test_typeahead_ingest_keeps_last_submitted_line_with_trailing_return},
    {"typeahead_ingest_returns_only_clear_pending_input",
     test_typeahead_ingest_returns_only_clear_pending_input},
    {"typeahead_ingest_line_feeds_only_remain_editable",
     test_typeahead_ingest_line_feeds_only_remain_editable},
    {"typeahead_ingest_chunked_ctrl_j_then_return",
     test_typeahead_ingest_chunked_ctrl_j_then_return},
    {"typeahead_prepare_clears_raw_without_controls_and_keeps_initial_input",
     test_typeahead_prepare_clears_raw_without_controls_and_keeps_initial_input},
    {"typeahead_prepare_replays_control_raw_bytes_in_original_order",
     test_typeahead_prepare_replays_control_raw_bytes_in_original_order},
    {"typeahead_prepare_failed_control_replay_preserves_initial_input",
     test_typeahead_prepare_failed_control_replay_preserves_initial_input},
    {"typeahead_pending_input_hidden_when_disabled",
     test_typeahead_pending_input_hidden_when_disabled},
    {"prompt_marker_roundtrip", test_prompt_marker_roundtrip},
    {"menu_prompt_api_roundtrip", test_menu_prompt_api_roundtrip},
    {"hint_delay_clamps", test_hint_delay_clamps},
    {"idle_timeout_setting", test_idle_timeout_setting},
    {"status_hint_mode_validation", test_status_hint_mode_validation},
    {"mouse_reporting_option_toggles", test_mouse_reporting_option_toggles},
    {"option_toggle_consistency", test_option_toggle_consistency},
    {"brace_pair_setters_validation", test_brace_pair_setters_validation},
    {"abbreviation_management", test_abbreviation_management},
    {"key_spec_parse_and_format_roundtrip", test_key_spec_parse_and_format_roundtrip},
    {"key_binding_crud_and_profiles", test_key_binding_crud_and_profiles},
    {"key_binding_profile_specs_register_all_bindings",
     test_key_binding_profile_specs_register_all_bindings},
    {"key_action_name_mappings", test_key_action_name_mappings},
    {"string_copy_bounds", test_string_copy_bounds},
    {"string_matching_and_token_helpers", test_string_matching_and_token_helpers},
    {"prev_next_char_utf8_helpers", test_prev_next_char_utf8_helpers},
    {"term_visibility_tracking_with_escape_and_control_bytes",
     test_term_visibility_tracking_with_escape_and_control_bytes},
    {"term_visibility_tracking_escape_only_sequences",
     test_term_visibility_tracking_escape_only_sequences},
    {"term_visibility_tracking_control_bytes_only",
     test_term_visibility_tracking_control_bytes_only},
    {"term_visibility_tracking_carriage_return_preserves_visible_state",
     test_term_visibility_tracking_carriage_return_preserves_visible_state},
    {"term_visibility_tracking_bracketed_paste_toggle_sequences",
     test_term_visibility_tracking_bracketed_paste_toggle_sequences},
    {"term_visibility_tracking_multiline_last_line_only",
     test_term_visibility_tracking_multiline_last_line_only},
    {"term_cursor_start_tracking_transitions", test_term_cursor_start_tracking_transitions},
    {"term_visibility_tracking_escape_whitespace_mix",
     test_term_visibility_tracking_escape_whitespace_mix},
    {"tty_bracketed_paste_enter_translation_flow", test_tty_bracketed_paste_enter_translation_flow},
    {"tty_bracketed_paste_repeated_start_without_end",
     test_tty_bracketed_paste_repeated_start_without_end},
    {"tty_bracketed_paste_repeated_end_without_start",
     test_tty_bracketed_paste_repeated_end_without_start},
    {"tty_sgr_mouse_event_metadata", test_tty_sgr_mouse_event_metadata},
    {"tty_focus_event_decode", test_tty_focus_event_decode},
    {"tty_kitty_ctrl_sequence_decode_regression", test_tty_kitty_ctrl_sequence_decode_regression},
    {"key_spec_ctrl_space_variants", test_key_spec_ctrl_space_variants},
    {"initial_input_env_lifecycle", test_initial_input_env_lifecycle},
    {"unicode_display_width_control_codepoints", test_unicode_display_width_control_codepoints},
    {"unicode_display_width_osc_sequence_ignored", test_unicode_display_width_osc_sequence_ignored},
    {"history_search_direction_and_position", test_history_search_direction_and_position},
    {"history_fuzzy_metadata_filtering", test_history_fuzzy_metadata_filtering},
    {"history_frequency_metadata_tracking", test_history_frequency_metadata_tracking},
    {"history_frequency_metadata_interactive_flow",
     test_history_frequency_metadata_interactive_flow},
    {"history_snapshot_dedup_keeps_latest_entry", test_history_snapshot_dedup_keeps_latest_entry},
    {"history_dedup_order_and_metadata", test_history_dedup_order_and_metadata},
    {"history_disabled_mode_rejects_push", test_history_disabled_mode_rejects_push},
    {"key_spec_separator_and_invalid_forms", test_key_spec_separator_and_invalid_forms},
    {"key_binding_named_invalid_inputs", test_key_binding_named_invalid_inputs},
    {"tty_code_pushback_order_and_capacity_guard", test_tty_code_pushback_order_and_capacity_guard},
    {"term_manual_visibility_override_api", test_term_manual_visibility_override_api},
    {"prompt_line_replacement_gate_matrix", test_prompt_line_replacement_gate_matrix},
    {"bbcode_default_styles_are_registered", test_bbcode_default_styles_are_registered},
    {"history_update_and_remove_last_flow", test_history_update_and_remove_last_flow},
    {"history_snapshot_bounds_and_order", test_history_snapshot_bounds_and_order},
    {"history_search_prefix_empty_query_behavior", test_history_search_prefix_empty_query_behavior},
    {"unicode_display_width_malformed_csi_sequences",
     test_unicode_display_width_malformed_csi_sequences},
    {"unicode_display_width_invalid_utf8_fallback",
     test_unicode_display_width_invalid_utf8_fallback},
    {"history_max_entries_pruning", test_history_max_entries_pruning},
    {"history_snapshot_load_dedup_mode", test_history_snapshot_load_dedup_mode},
    {"history_remove_last_on_empty_safe", test_history_remove_last_on_empty_safe},
    {"history_fuzzy_max_matches_cap", test_history_fuzzy_max_matches_cap},
    {"key_spec_alias_and_f24_roundtrip", test_key_spec_alias_and_f24_roundtrip},
    {"key_queue_api_noop_inputs", test_key_queue_api_noop_inputs},
    {"command_palette_entry_registration_and_listing",
     test_command_palette_entry_registration_and_listing},
    {"command_palette_handler_registration", test_command_palette_handler_registration},
    {"custom_menu_rejects_calls_without_active_editor",
     test_custom_menu_rejects_calls_without_active_editor},
    {"status_message_callback_registration", test_status_message_callback_registration},
    {"term_color_bits_and_toggle_roundtrip", test_term_color_bits_and_toggle_roundtrip},
};

int main(void) {
    size_t failures = 0;
    const size_t test_count = sizeof(kTests) / sizeof(kTests[0]);

    for (size_t i = 0; i < test_count; ++i) {
        if (!kTests[i].fn()) {
            expect_safe_log("Test '%s' failed\n", kTests[i].name);
            failures += 1;
        }
    }

    if (failures > 0) {
        expect_safe_log("%zu/%zu isocline behavior tests failed\n", failures, test_count);
        return 1;
    }

    (void)printf("All %zu isocline behavior tests passed\n", test_count);
    return 0;
}
