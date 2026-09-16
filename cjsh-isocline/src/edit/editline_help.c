/*
  editline_help.c

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

//-------------------------------------------------------------
// Help: this is included into editline.c
//-------------------------------------------------------------

#include <string.h>

#include "common.h"
#include "isocline.h"

typedef enum help_line_type_e {
    HELP_LINE_BLANK,
    HELP_LINE_HEADING,
    HELP_LINE_BINDING,
    HELP_LINE_STATIC
} help_line_type_t;

typedef struct help_line_s {
    help_line_type_t type;
    ic_key_action_t action;
    const char* text;
    const char* description;
    const char* default_specs;
} help_line_t;

static const help_line_t help_lines[] = {
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "Navigation:", NULL, NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_LEFT, NULL, "go one character to the left", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, NULL,
     "go one character to the right", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_UP, NULL, "go one row up, or back in the history",
     NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_DOWN, NULL,
     "go one row down, or forward in the history", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_WORD_PREV, NULL,
     "go to the start of the previous word", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, NULL,
     "go to the end of the current word", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_LINE_START, NULL,
     "go to the start of the current line", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_LINE_END, NULL, "go to the end of the current line",
     NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_INPUT_START, NULL,
     "go to the start of the current input", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_INPUT_END, NULL, "go to the end of the current input",
     NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CURSOR_MATCH_BRACE, NULL, "jump to matching brace", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_HISTORY_PREV, NULL, "go back in the history", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_HISTORY_NEXT, NULL, "go forward in the history", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_HISTORY_SEARCH, NULL,
     "search the history starting with the current word", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_COMMAND_PALETTE, NULL, "open the command palette for actions",
     NULL},
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "Deletion:", NULL, NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_FORWARD, NULL, "delete the current character", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_BACKWARD, NULL, "delete the previous character", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_WORD_START_WS, NULL, "delete to preceding white space",
     NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_WORD_START, NULL,
     "delete to the start of the current word", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_WORD_END, NULL,
     "delete to the end of the current word", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_LINE_START, NULL,
     "delete to the start of the current line", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_DELETE_LINE_END, NULL,
     "delete to the end of the current line", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "esc",
     "delete the current input, or done with empty input", NULL},
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "Editing:", NULL, NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "enter", "accept current input", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_INSERT_NEWLINE, NULL,
     "create a new line for multi-line input", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_CLEAR_SCREEN, NULL, "clear screen", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_TRANSPOSE_CHARS, NULL,
     "swap with previous character (move character backward)", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_YANK_LAST_ARG, NULL,
     "insert the last argument from the previous command", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_UNDO, NULL, "undo", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_REDO, NULL, "redo", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_COMPLETE, NULL, "try to complete the current input", NULL},
    {HELP_LINE_BINDING, IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING, NULL,
     "toggle mouse reporting for this prompt", NULL},
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "In the completion menu:", NULL, NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "enter,right", "use the currently selected completion",
     NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "1 - 9", "use completion N from the menu", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "tab,down", "select the next completion", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "shift-tab,up", "select the previous completion", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "esc", "exit menu without completing", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "pgup,pgdn",
     "page through completions (pgdn loads more)", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "shift-up,shift-down", "page through completions", NULL},
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "In the history search menu:", NULL, NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "enter", "run the selected history entry", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "tab", "edit the selected history entry", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "up,down", "move selection", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "shift-up,shift-down", "page through history", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "alt-c", "toggle case-sensitive matching", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "alt-d", "toggle current-directory scope", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "alt-n", "toggle nested directories", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "alt-s", "cycle history sorting", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "esc", "exit search", NULL},
    {HELP_LINE_BLANK, IC_KEY_ACTION__MAX, NULL, NULL, NULL},
    {HELP_LINE_HEADING, IC_KEY_ACTION__MAX, "In the command palette:", NULL, NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "enter,tab", "run the selected action", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "up,down", "move selection", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "shift-up,shift-down", "page through actions", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "alt-c", "toggle case-sensitive matching", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, "esc", "exit palette", NULL},
    {HELP_LINE_STATIC, IC_KEY_ACTION__MAX, " ", "", NULL},
};

static const char* help_initial =
    "[ic-info]"
    "Isocline v1.0, copyright (c) 2021 Daan Leijen.\n"
    "Largely Modified by Caden Finley 2025 for CJ's Shell.\n"
    "This distribution of isocline is a fork of <[url]https://github.com/daanx/isocline[/url]>\n"
    "This is free software; you can redistribute it and/or\n"
    "modify it under the terms of the MIT License.\n"
    "See <[url]https://github.com/cadenfinley/isocline[/url]> for further "
    "information.\n"
    "We use ^<key> as a shorthand for ctrl-<key>.\n"
    "\n"
    "Overview:\n"
    "\n[ansi-lightgray]"
    "       home,ctrl-a      cursor     end,ctrl-e\n"
    "         ┌────────────────┼───────────────┐    (navigate)\n"
//"       │                │               │\n"
#ifndef __APPLE__
    "         │    ctrl-left   │  ctrl-right   │\n"
#else
    "         │     alt-left   │   alt-right   │\n"
#endif
    "         │        ┌───────┼──────┐        │    ctrl-r   : search history\n"
    "         ▼        ▼       ▼      ▼        ▼    tab      : complete word\n"
    "  prompt> [ansi-darkgray]it's the quintessential language[/]     "
    "shift-tab: insert new line\n"
    "         ▲        ▲              ▲        ▲    esc      : delete input, "
    "done\n"
    "         │        └──────────────┘        │    ctrl-z   : undo\n"
    "         │   alt-backsp        alt-d      │\n"
    //"       │                │               │\n"
    "         └────────────────────────────────┘    (delete)\n"
    "       ctrl-u                          ctrl-k\n"
    "[/ansi-lightgray][/ic-info]\n";

static bool key_triggers_action(ic_env_t* env, ic_keycode_t key, ic_key_action_t action) {
    ic_unused(env);
    ic_key_action_t configured;
    if (ic_get_key_binding(key, &configured)) {
        return configured == action;
    }
    return true;
}

static void beautify_key_label(char* label) {
    if (label == NULL) {
        return;
    }
    size_t len = strlen(label);
    if (len >= 6 && ic_strnicmp(label, "ctrl+", 5) == 0 && strchr(label + 5, '+') == NULL &&
        strlen(label + 5) == 1) {
        label[0] = '^';
        label[1] = label[5];
        label[2] = '\0';
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        if (label[i] == '+') {
            label[i] = '-';
        }
    }
}

static bool format_first_default_binding(ic_env_t* env, ic_key_action_t action,
                                         const char* default_specs, char* buffer, size_t buflen) {
    if (buffer == NULL || buflen == 0) {
        return false;
    }

    const char* specs_to_use = default_specs;
    if ((specs_to_use == NULL || specs_to_use[0] == '\0') && action > IC_KEY_ACTION_NONE &&
        action < IC_KEY_ACTION__MAX) {
        specs_to_use = ic_key_binding_profile_default_specs(action);
    }
    if (specs_to_use == NULL || specs_to_use[0] == '\0') {
        return false;
    }

    size_t len = strlen(specs_to_use);
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (specs_to_use[i] == '|' || specs_to_use[i] == '\0') {
            size_t tok_len = i - start;
            if (tok_len > 0 && tok_len < 64) {
                char token[64];
                memcpy(token, specs_to_use + start, tok_len);
                token[tok_len] = '\0';
                size_t left = 0;
                while (token[left] == ' ') {
                    left++;
                }
                size_t right = strlen(token);
                while (right > left && token[right - 1] == ' ') {
                    right--;
                }
                token[right] = '\0';
                if (right > left) {
                    const char* trimmed = token + left;
                    ic_keycode_t key;
                    if (ic_parse_key_spec(trimmed, &key) && key_triggers_action(env, key, action) &&
                        ic_format_key_spec(key, buffer, buflen)) {
                        beautify_key_label(buffer);
                        return true;
                    }
                }
            }
            start = i + 1;
        }
    }
    return false;
}

static bool format_first_custom_binding(ic_env_t* env, ic_key_action_t action, char* buffer,
                                        size_t buflen) {
    if (env == NULL || buffer == NULL || buflen == 0) {
        return false;
    }
    if (env->key_binding_count <= 0 || env->key_bindings == NULL) {
        return false;
    }
    for (ssize_t i = 0; i < env->key_binding_count; ++i) {
        ic_key_binding_entry_t entry = env->key_bindings[i];
        if (entry.action != action) {
            continue;
        }
        if (ic_format_key_spec(entry.key, buffer, buflen)) {
            beautify_key_label(buffer);
            return true;
        }
    }
    return false;
}

#define HELP_MAX_LABELS 16
#define HELP_LABEL_LEN 64

static bool key_label_equals(const char* a, const char* b) {
    return (ic_stricmp(a, b) == 0);
}

static bool help_label_exists(char labels[][HELP_LABEL_LEN], size_t count, const char* label) {
    for (size_t i = 0; i < count; ++i) {
        if (key_label_equals(labels[i], label)) {
            return true;
        }
    }
    return false;
}

static void help_label_add(char labels[][HELP_LABEL_LEN], size_t* count, const char* label) {
    if (label == NULL || labels == NULL || count == NULL) {
        return;
    }
    if (*count >= HELP_MAX_LABELS) {
        return;
    }
    if (help_label_exists(labels, *count, label)) {
        return;
    }
    (void)ic_strncpy(labels[*count], HELP_LABEL_LEN, label, HELP_LABEL_LEN - 1);
    beautify_key_label(labels[*count]);
    (*count)++;
}

static void format_binding_keys(ic_env_t* env, ic_key_action_t action, const char* default_specs,
                                char* buffer, size_t buflen, bool status_hint_mode) {
    if (buffer == NULL || buflen == 0) {
        return;
    }

    if (status_hint_mode) {
        if (format_first_custom_binding(env, action, buffer, buflen)) {
            return;
        }
        if (format_first_default_binding(env, action, default_specs, buffer, buflen)) {
            return;
        }
        (void)ic_strncpy(buffer, (ssize_t)buflen, "(unbound)", (ssize_t)buflen - 1);
        return;
    }
    char labels[HELP_MAX_LABELS][HELP_LABEL_LEN];
    size_t label_count = 0;

    const char* specs_to_use = default_specs;
    if ((specs_to_use == NULL || specs_to_use[0] == '\0') && action > IC_KEY_ACTION_NONE &&
        action < IC_KEY_ACTION__MAX) {
        specs_to_use = ic_key_binding_profile_default_specs(action);
    }

    if (specs_to_use != NULL && specs_to_use[0] != '\0') {
        const char* spec = specs_to_use;
        size_t len = strlen(specs_to_use);
        size_t start = 0;
        for (size_t i = 0; i <= len; ++i) {
            if (specs_to_use[i] == '|' || specs_to_use[i] == '\0') {
                size_t tok_len = i - start;
                if (tok_len > 0 && tok_len < 64) {
                    char token[64];
                    memcpy(token, spec + start, tok_len);
                    token[tok_len] = '\0';
                    // trim spaces
                    size_t left = 0;
                    while (token[left] == ' ') {
                        left++;
                    }
                    size_t right = strlen(token);
                    while (right > left && token[right - 1] == ' ') {
                        right--;
                    }
                    token[right] = '\0';
                    if (right > left) {
                        const char* trimmed = token + left;
                        ic_keycode_t key;
                        if (ic_parse_key_spec(trimmed, &key) &&
                            key_triggers_action(env, key, action)) {
                            char formatted[64];
                            if (ic_format_key_spec(key, formatted, sizeof(formatted))) {
                                help_label_add(labels, &label_count, formatted);
                            }
                        }
                    }
                }
                start = i + 1;
            }
        }
    }

    if (env != NULL && env->key_binding_count > 0 && env->key_bindings != NULL) {
        for (ssize_t i = 0; i < env->key_binding_count; ++i) {
            ic_key_binding_entry_t entry = env->key_bindings[i];
            if (entry.action != action) {
                continue;
            }
            char formatted[64];
            if (ic_format_key_spec(entry.key, formatted, sizeof(formatted))) {
                help_label_add(labels, &label_count, formatted);
            }
        }
    }

    if (label_count == 0) {
        (void)ic_strncpy(buffer, (ssize_t)buflen, "(unbound)", (ssize_t)buflen - 1);
        return;
    }

    buffer[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < label_count; ++i) {
        int written = snprintf(buffer + pos, buflen - pos, "%s%s", (i == 0 ? "" : ", "), labels[i]);
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
        if (pos >= buflen) {
            break;
        }
    }
}

static void edit_show_help(ic_env_t* env, editor_t* eb) {
    edit_clear(env, eb);
    bbcode_println(env->bbcode, help_initial);

    const size_t line_count = sizeof(help_lines) / sizeof(help_lines[0]);
    for (size_t i = 0; i < line_count; ++i) {
        const help_line_t* line = &help_lines[i];
        switch (line->type) {
            case HELP_LINE_BLANK:
                bbcode_println(env->bbcode, "");
                break;
            case HELP_LINE_HEADING:
                bbcode_printf(env->bbcode, "[ic-info]%s[/]\n", line->text);
                break;
            case HELP_LINE_STATIC:
                bbcode_printf(env->bbcode, "  [ic-emphasis]%-13s[/][ansi-lightgray]%s%s[/]\n",
                              line->text, (line->description[0] == 0 ? "" : ": "),
                              line->description);
                break;
            case HELP_LINE_BINDING: {
                char key_buffer[256];
                char mouse_suffix[320];
                format_binding_keys(env, line->action, line->default_specs, key_buffer,
                                    sizeof(key_buffer), false);
                bool mouse_toggle_enabled = (line->action == IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING &&
                                             eb != NULL && eb->mouse_reporting_enabled);
                mouse_suffix[0] = '\0';
                if (mouse_toggle_enabled) {
                    if (strcmp(key_buffer, "(unbound)") != 0) {
                        (void)snprintf(mouse_suffix, sizeof(mouse_suffix),
                                       " (Mouse clicking is enabled; press %s to disable)",
                                       key_buffer);
                    } else {
                        (void)ic_strncpy(mouse_suffix, (ssize_t)sizeof(mouse_suffix),
                                         " (Mouse clicking is enabled)",
                                         (ssize_t)sizeof(mouse_suffix) - 1);
                    }
                }
                bbcode_printf(env->bbcode, "  [ic-emphasis]%-13s[/][ansi-lightgray]%s%s%s[/]\n",
                              key_buffer, (line->description[0] == 0 ? "" : ": "),
                              line->description, mouse_suffix);
                break;
            }
        }
    }

    if (eb->prompt_prefix_lines > 0) {
        redraw_prompt_prefix_lines(env, eb);
    }
    eb->cur_rows = 0;
    eb->input_rows = 0;
    eb->cur_row = 0;
    eb->view_first_row = 0;
    eb->view_rows = 0;
    eb->view_input_rows = 0;
    edit_refresh(env, eb);
}
