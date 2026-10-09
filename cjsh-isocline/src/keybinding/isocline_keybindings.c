/*
  isocline_keybindings.c

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
    Key binding related public APIs and profile helpers extracted from the
    original monolithic isocline.c translation unit.
-----------------------------------------------------------------------------*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"
#include "env.h"
#include "keybinding_internal.h"
#include "keybinding_specs.h"
#include "keybindings.h"
#include "keycodes.h"

//-------------------------------------------------------------
// Key binding helpers
//-------------------------------------------------------------

typedef struct key_action_name_entry_s {
    const char* name;
    ic_key_action_t action;
} key_action_name_entry_t;

static const key_action_name_entry_t key_action_names[] = {
    {"none", IC_KEY_ACTION_NONE},
    {"suppress", IC_KEY_ACTION_NONE},

    {"complete", IC_KEY_ACTION_COMPLETE},
    {"completion", IC_KEY_ACTION_COMPLETE},

    {"history-search", IC_KEY_ACTION_HISTORY_SEARCH},
    {"search-history", IC_KEY_ACTION_HISTORY_SEARCH},

    {"history-prev", IC_KEY_ACTION_HISTORY_PREV},
    {"history-up", IC_KEY_ACTION_HISTORY_PREV},

    {"history-next", IC_KEY_ACTION_HISTORY_NEXT},
    {"history-down", IC_KEY_ACTION_HISTORY_NEXT},

    {"clear-screen", IC_KEY_ACTION_CLEAR_SCREEN},
    {"cls", IC_KEY_ACTION_CLEAR_SCREEN},

    {"undo", IC_KEY_ACTION_UNDO},
    {"redo", IC_KEY_ACTION_REDO},

    {"show-help", IC_KEY_ACTION_SHOW_HELP},
    {"help", IC_KEY_ACTION_SHOW_HELP},

    {"cursor-left", IC_KEY_ACTION_CURSOR_LEFT},

    {"cursor-right-smart", IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE},
    {"cursor-right", IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE},

    {"cursor-up", IC_KEY_ACTION_CURSOR_UP},
    {"cursor-down", IC_KEY_ACTION_CURSOR_DOWN},

    {"cursor-line-start", IC_KEY_ACTION_CURSOR_LINE_START},
    {"cursor-line-end", IC_KEY_ACTION_CURSOR_LINE_END},

    {"cursor-word-prev", IC_KEY_ACTION_CURSOR_WORD_PREV},

    {"cursor-word-next-smart", IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE},
    {"cursor-word-next", IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE},

    {"cursor-input-start", IC_KEY_ACTION_CURSOR_INPUT_START},
    {"cursor-input-end", IC_KEY_ACTION_CURSOR_INPUT_END},
    {"cursor-match-brace", IC_KEY_ACTION_CURSOR_MATCH_BRACE},

    {"delete-backward", IC_KEY_ACTION_DELETE_BACKWARD},
    {"backspace", IC_KEY_ACTION_DELETE_BACKWARD},

    {"delete-forward", IC_KEY_ACTION_DELETE_FORWARD},
    {"delete", IC_KEY_ACTION_DELETE_FORWARD},

    {"delete-word-end", IC_KEY_ACTION_DELETE_WORD_END},
    {"kill-word", IC_KEY_ACTION_DELETE_WORD_END},

    {"delete-word-start-ws", IC_KEY_ACTION_DELETE_WORD_START_WS},
    {"backward-kill-word-ws", IC_KEY_ACTION_DELETE_WORD_START_WS},

    {"delete-word-start", IC_KEY_ACTION_DELETE_WORD_START},
    {"backward-kill-word", IC_KEY_ACTION_DELETE_WORD_START},

    {"delete-line-start", IC_KEY_ACTION_DELETE_LINE_START},
    {"delete-line-end", IC_KEY_ACTION_DELETE_LINE_END},

    {"transpose-chars", IC_KEY_ACTION_TRANSPOSE_CHARS},
    {"swap-chars", IC_KEY_ACTION_TRANSPOSE_CHARS},

    {"yank-last-arg", IC_KEY_ACTION_YANK_LAST_ARG},
    {"insert-last-argument", IC_KEY_ACTION_YANK_LAST_ARG},

    {"insert-newline", IC_KEY_ACTION_INSERT_NEWLINE},
    {"newline", IC_KEY_ACTION_INSERT_NEWLINE},

    {"toggle-mouse-reporting", IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING},
    {"mouse-reporting-toggle", IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING},
    {"toggle-mouse", IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING},

    {"command-palette", IC_KEY_ACTION_COMMAND_PALETTE},
    {"palette", IC_KEY_ACTION_COMMAND_PALETTE},
    {"action-palette", IC_KEY_ACTION_COMMAND_PALETTE},
    {"search-actions", IC_KEY_ACTION_COMMAND_PALETTE},

    {"runoff", IC_KEY_ACTION_RUNOFF},
    {"runoff-handler", IC_KEY_ACTION_RUNOFF},
    {"unhandled", IC_KEY_ACTION_RUNOFF},
};

static size_t key_action_name_count(void) {
    return sizeof(key_action_names) / sizeof(key_action_names[0]);
}

typedef struct keybinding_profile_action_spec_s {
    ic_key_action_t action;
    const char* specs;
} keybinding_profile_action_spec_t;

typedef struct keybinding_profile_binding_s {
    ic_key_action_t action;
    const char* specs;
} keybinding_profile_binding_t;

typedef struct ic_keybinding_profile_s {
    const char* name;
    const char* description;
    const struct ic_keybinding_profile_s* parent;
    const keybinding_profile_action_spec_t* specs;
    size_t spec_count;
    const keybinding_profile_binding_t* bindings;
    size_t binding_count;
} keybinding_profile_t;

static const keybinding_profile_action_spec_t keybinding_profile_default_spec_entries[] = {
    {IC_KEY_ACTION_CURSOR_LEFT, SPEC_CURSOR_LEFT},
    {IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, SPEC_CURSOR_RIGHT},
    {IC_KEY_ACTION_CURSOR_UP, SPEC_CURSOR_UP},
    {IC_KEY_ACTION_CURSOR_DOWN, SPEC_CURSOR_DOWN},
    {IC_KEY_ACTION_CURSOR_WORD_PREV, SPEC_CURSOR_WORD_PREV},
    {IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, SPEC_CURSOR_WORD_NEXT},
    {IC_KEY_ACTION_CURSOR_LINE_START, SPEC_CURSOR_LINE_START},
    {IC_KEY_ACTION_CURSOR_LINE_END, SPEC_CURSOR_LINE_END},
    {IC_KEY_ACTION_CURSOR_INPUT_START, SPEC_CURSOR_INPUT_START},
    {IC_KEY_ACTION_CURSOR_INPUT_END, SPEC_CURSOR_INPUT_END},
    {IC_KEY_ACTION_CURSOR_MATCH_BRACE, SPEC_CURSOR_MATCH_BRACE},
    {IC_KEY_ACTION_HISTORY_PREV, SPEC_HISTORY_PREV},
    {IC_KEY_ACTION_HISTORY_NEXT, SPEC_HISTORY_NEXT},
    {IC_KEY_ACTION_HISTORY_SEARCH, SPEC_HISTORY_SEARCH},
    {IC_KEY_ACTION_DELETE_FORWARD, SPEC_DELETE_FORWARD},
    {IC_KEY_ACTION_DELETE_BACKWARD, SPEC_DELETE_BACKWARD},
    {IC_KEY_ACTION_DELETE_WORD_START_WS, SPEC_DELETE_WORD_START_WS},
    {IC_KEY_ACTION_DELETE_WORD_START, SPEC_DELETE_WORD_START},
    {IC_KEY_ACTION_DELETE_WORD_END, SPEC_DELETE_WORD_END},
    {IC_KEY_ACTION_DELETE_LINE_START, SPEC_DELETE_LINE_START},
    {IC_KEY_ACTION_DELETE_LINE_END, SPEC_DELETE_LINE_END},
    {IC_KEY_ACTION_TRANSPOSE_CHARS, SPEC_TRANSPOSE},
    {IC_KEY_ACTION_YANK_LAST_ARG, SPEC_YANK_LAST_ARG},
    {IC_KEY_ACTION_CLEAR_SCREEN, SPEC_CLEAR_SCREEN},
    {IC_KEY_ACTION_UNDO, SPEC_UNDO},
    {IC_KEY_ACTION_REDO, SPEC_REDO},
    {IC_KEY_ACTION_COMPLETE, SPEC_COMPLETE},
    {IC_KEY_ACTION_SHOW_HELP, SPEC_SHOW_HELP},
    {IC_KEY_ACTION_INSERT_NEWLINE, SPEC_INSERT_NEWLINE},
    {IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING, SPEC_TOGGLE_MOUSE_REPORTING},
    {IC_KEY_ACTION_COMMAND_PALETTE, SPEC_COMMAND_PALETTE},
};

static const keybinding_profile_t keybinding_profile_default = {
    "emacs",
    "Emacs-style bindings (default)",
    NULL,
    keybinding_profile_default_spec_entries,
    sizeof(keybinding_profile_default_spec_entries) /
        sizeof(keybinding_profile_default_spec_entries[0]),
    NULL,
    0,
};

static const keybinding_profile_action_spec_t keybinding_profile_vim_spec_entries[] = {
    {IC_KEY_ACTION_CURSOR_LEFT, SPEC_CURSOR_LEFT "|alt+h"},
    {IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, SPEC_CURSOR_RIGHT "|alt+l"},
    {IC_KEY_ACTION_CURSOR_UP, SPEC_CURSOR_UP "|alt+k"},
    {IC_KEY_ACTION_CURSOR_DOWN, SPEC_CURSOR_DOWN "|alt+j"},
    {IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, SPEC_CURSOR_WORD_NEXT "|alt+w"},
};

static const keybinding_profile_binding_t keybinding_profile_vim_bindings[] = {
    {IC_KEY_ACTION_CURSOR_LEFT, "alt+h"},
    {IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, "alt+l"},
    {IC_KEY_ACTION_CURSOR_UP, "alt+k"},
    {IC_KEY_ACTION_CURSOR_DOWN, "alt+j"},
    {IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, "alt+w"},
};

static const keybinding_profile_t keybinding_profile_vim = {
    "vim",
    "Vim-inspired navigation bindings (Alt+H/J/K/L, Alt+W)",
    &keybinding_profile_default,
    keybinding_profile_vim_spec_entries,
    sizeof(keybinding_profile_vim_spec_entries) / sizeof(keybinding_profile_vim_spec_entries[0]),
    keybinding_profile_vim_bindings,
    sizeof(keybinding_profile_vim_bindings) / sizeof(keybinding_profile_vim_bindings[0]),
};

static const keybinding_profile_t* const keybinding_profiles[] = {
    &keybinding_profile_default,
    &keybinding_profile_vim,
};

static size_t keybinding_profile_count(void) {
    return sizeof(keybinding_profiles) / sizeof(keybinding_profiles[0]);
}

static ic_key_binding_entry_t* key_binding_find_entry(ic_env_t* env, ic_keycode_t key,
                                                      ssize_t* index_out) {
    if (env == NULL || env->key_bindings == NULL) {
        return NULL;
    }
    for (ssize_t i = 0; i < env->key_binding_count; ++i) {
        if (env->key_bindings[i].key == key) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return &env->key_bindings[i];
        }
    }
    return NULL;
}

static bool key_bindings_ensure_capacity(ic_env_t* env, ssize_t needed) {
    if (env->key_binding_capacity >= needed) {
        return true;
    }
    ssize_t new_capacity = (env->key_binding_capacity == 0 ? 8 : env->key_binding_capacity * 2);
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ic_key_binding_entry_t* resized =
        mem_realloc_tp(env->mem, ic_key_binding_entry_t, env->key_bindings, new_capacity);
    if (resized == NULL) {
        return false;
    }
    env->key_bindings = resized;
    env->key_binding_capacity = new_capacity;
    return true;
}

typedef struct key_name_entry_s {
    const char* name;
    ic_keycode_t key;
} key_name_entry_t;

static const key_name_entry_t key_name_map[] = {
    {"tab", IC_KEY_TAB},
    {"enter", IC_KEY_ENTER},
    {"return", IC_KEY_ENTER},
    {"linefeed", IC_KEY_LINEFEED},
    {"lf", IC_KEY_LINEFEED},
    {"backspace", IC_KEY_BACKSP},
    {"bs", IC_KEY_BACKSP},
    {"delete", IC_KEY_DEL},
    {"del", IC_KEY_DEL},
    {"insert", IC_KEY_INS},
    {"ins", IC_KEY_INS},
    {"escape", IC_KEY_ESC},
    {"esc", IC_KEY_ESC},
    {"space", IC_KEY_SPACE},
    {"left", IC_KEY_LEFT},
    {"right", IC_KEY_RIGHT},
    {"up", IC_KEY_UP},
    {"down", IC_KEY_DOWN},
    {"home", IC_KEY_HOME},
    {"end", IC_KEY_END},
    {"pageup", IC_KEY_PAGEUP},
    {"pgup", IC_KEY_PAGEUP},
    {"pagedown", IC_KEY_PAGEDOWN},
    {"pgdn", IC_KEY_PAGEDOWN},
    {"f1", IC_KEY_F1},
    {"f2", IC_KEY_F2},
    {"f3", IC_KEY_F3},
    {"f4", IC_KEY_F4},
    {"f5", IC_KEY_F5},
    {"f6", IC_KEY_F6},
    {"f7", IC_KEY_F7},
    {"f8", IC_KEY_F8},
    {"f9", IC_KEY_F9},
    {"f10", IC_KEY_F10},
    {"f11", IC_KEY_F11},
    {"f12", IC_KEY_F12},
};

static const keybinding_profile_t* keybinding_profile_lookup(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < keybinding_profile_count(); ++i) {
        const keybinding_profile_t* profile = keybinding_profiles[i];
        if (profile != NULL && ic_stricmp(name, profile->name) == 0) {
            return profile;
        }
    }
    return NULL;
}

static const char* keybinding_profile_find_spec(const keybinding_profile_t* profile,
                                                ic_key_action_t action) {
    if (profile == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < profile->spec_count; ++i) {
        if (profile->specs[i].action == action) {
            return profile->specs[i].specs;
        }
    }
    return keybinding_profile_find_spec(profile->parent, action);
}

static bool keybinding_profile_bind_string(ic_env_t* env, ic_key_action_t action,
                                           const char* specs) {
    if (env == NULL || specs == NULL) {
        return true;
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
        if (len >= 64) {
            return false;
        }
        char token[64];
        memcpy(token, start, len);
        token[len] = '\0';
        ic_keycode_t key;
        if (!ic_parse_key_spec(token, &key)) {
            return false;
        }
        if (!ic_bind_key(key, action)) {
            return false;
        }
    }
    return true;
}

static bool keybinding_profile_apply_recursive(ic_env_t* env, const keybinding_profile_t* profile) {
    if (env == NULL || profile == NULL) {
        return true;
    }
    if (!keybinding_profile_apply_recursive(env, profile->parent)) {
        return false;
    }
    for (size_t i = 0; i < profile->binding_count; ++i) {
        const keybinding_profile_binding_t* binding = &profile->bindings[i];
        if (!keybinding_profile_bind_string(env, binding->action, binding->specs)) {
            return false;
        }
    }
    return true;
}

ic_private bool ic_keybinding_apply_profile(ic_env_t* env, const keybinding_profile_t* profile) {
    return keybinding_profile_apply_recursive(env, profile);
}

static void key_binding_clear_all(ic_env_t* env) {
    if (env == NULL) {
        return;
    }
    env->key_binding_count = 0;
}

static bool key_lookup_named(const char* token, ic_keycode_t* out_key) {
    for (size_t i = 0; i < sizeof(key_name_map) / sizeof(key_name_map[0]); ++i) {
        if (ic_stricmp(token, key_name_map[i].name) == 0) {
            *out_key = key_name_map[i].key;
            return true;
        }
    }
    // F-keys beyond explicit table
    if (token[0] == 'f' || token[0] == 'F') {
        char* endptr = NULL;
        long number = strtol(token + 1, &endptr, 10);
        if (endptr != token + 1 && *endptr == '\0' && number >= 1 && number <= 24) {
            *out_key = (ic_keycode_t)(IC_KEY_F1 + (number - 1));
            return true;
        }
    }
    return false;
}

static bool append_token(bool* first, char* buffer, size_t buflen, size_t* len, const char* token) {
    size_t token_len = strlen(token);
    size_t extra = (*first ? 0 : 1);
    if (*len + extra + token_len + 1 > buflen) {
        return false;
    }
    if (!*first) {
        buffer[*len] = '+';
        (*len)++;
    }
    memcpy(buffer + *len, token, token_len);
    *len += token_len;
    buffer[*len] = '\0';
    *first = false;
    return true;
}

//-------------------------------------------------------------
// Internal helpers exposed to other modules
//-------------------------------------------------------------

ic_private const struct ic_keybinding_profile_s* ic_keybinding_profile_default_ptr(void) {
    return &keybinding_profile_default;
}

//-------------------------------------------------------------
// Public API
//-------------------------------------------------------------

ic_public bool ic_parse_key_spec(const char* spec, ic_keycode_t* out_key) {
    if (spec == NULL || out_key == NULL) {
        return false;
    }
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool base_set = false;
    bool base_is_char = false;
    char base_char = '\0';
    ic_keycode_t base_key = 0;

    char token[32];
    size_t tok_len = 0;
    for (const char* p = spec;; ++p) {
        char ch = *p;
        bool at_end = (ch == '\0');
        if (at_end || ch == '+' || ch == '-' || ch == ' ' || ch == '\t') {
            if (tok_len > 0) {
                token[tok_len] = '\0';
                for (size_t i = 0; i < tok_len; ++i) {
                    token[i] = (char)tolower((unsigned char)token[i]);
                }
                if (strcmp(token, "ctrl") == 0 || strcmp(token, "control") == 0) {
                    ctrl = true;
                } else if (strcmp(token, "alt") == 0 || strcmp(token, "meta") == 0 ||
                           strcmp(token, "option") == 0) {
                    alt = true;
                } else if (strcmp(token, "shift") == 0) {
                    shift = true;
                } else {
                    if (base_set) {
                        return false;
                    }
                    if (tok_len == 1) {
                        base_is_char = true;
                        base_char = token[0];
                        base_set = true;
                    } else {
                        ic_keycode_t named;
                        if (key_lookup_named(token, &named)) {
                            base_key = named;
                            base_is_char = false;
                            base_set = true;
                        } else if (strcmp(token, "newline") == 0) {
                            base_key = IC_KEY_LINEFEED;
                            base_is_char = false;
                            base_set = true;
                        } else {
                            return false;
                        }
                    }
                }
                tok_len = 0;
            }
            if (at_end) {
                break;
            }
        } else {
            if (tok_len + 1 >= sizeof(token)) {
                return false;
            }
            token[tok_len++] = ch;
        }
    }

    if (!base_set) {
        return false;
    }

    ic_keycode_t code = 0;
    if (base_is_char) {
        unsigned char ch = (unsigned char)base_char;
        if (ctrl) {
            if (ch >= 'a' && ch <= 'z') {
                code = (ic_keycode_t)(IC_KEY_CTRL_A + (ch - 'a'));
                ctrl = false;
            } else if (ch >= 'A' && ch <= 'Z') {
                code = (ic_keycode_t)(IC_KEY_CTRL_A + (ch - 'A'));
                ctrl = false;
            } else if (ch == '@') {
                code = IC_KEY_CTRL_SPACE;
                ctrl = false;
            } else {
                code = IC_KEY_WITH_CTRL(ic_key_char((char)ch));
                ctrl = false;
            }
        } else {
            code = ic_key_char((char)ch);
        }
    } else {
        code = base_key;
    }

    if (ctrl) {
        code = IC_KEY_WITH_CTRL(code);
    }
    if (alt) {
        code = IC_KEY_WITH_ALT(code);
    }
    if (shift) {
        code = IC_KEY_WITH_SHIFT(code);
    }

    *out_key = code;
    return true;
}

ic_public bool ic_bind_key_named(const char* key_spec, const char* action_name) {
    if (key_spec == NULL || action_name == NULL) {
        return false;
    }
    ic_keycode_t key;
    if (!ic_parse_key_spec(key_spec, &key)) {
        return false;
    }
    ic_key_action_t action = ic_key_action_from_name(action_name);
    if (action == IC_KEY_ACTION__MAX) {
        return false;
    }
    return ic_bind_key(key, action);
}

ic_public bool ic_format_key_spec(ic_keycode_t key, char* buffer, size_t buflen) {
    if (buffer == NULL || buflen == 0) {
        return false;
    }
    buffer[0] = '\0';
    size_t len = 0;
    bool first = true;

    ic_keycode_t mods = IC_KEY_MODS(key);
    ic_keycode_t base = IC_KEY_NO_MODS(key);
    bool implicit_ctrl = false;
    bool prefer_named_ctrl_label = (base == IC_KEY_TAB);
    if ((mods & IC_KEY_MOD_CTRL) == 0 && !prefer_named_ctrl_label && base >= IC_KEY_CTRL_A &&
        base <= IC_KEY_CTRL_Z) {
        implicit_ctrl = true;
    }

    if (((mods & IC_KEY_MOD_CTRL) != 0 || implicit_ctrl) &&
        (!append_token(&first, buffer, buflen, &len, "ctrl"))) {
        return false;
    }

    if (mods & IC_KEY_MOD_ALT) {
        if (!append_token(&first, buffer, buflen, &len, "alt")) {
            return false;
        }
    }
    if (mods & IC_KEY_MOD_SHIFT) {
        if (!append_token(&first, buffer, buflen, &len, "shift")) {
            return false;
        }
    }

    char base_buf[16];
    const char* base_name = NULL;
    if (implicit_ctrl) {
        base_buf[0] = (char)('a' + (int)(base - IC_KEY_CTRL_A));
        base_buf[1] = '\0';
        base_name = base_buf;
    } else if (base >= IC_KEY_F1 && base <= IC_KEY_F1 + 23) {
        unsigned number = 1U + (base - IC_KEY_F1);
        if (number > 24) {
            return false;
        }
        (void)snprintf(base_buf, sizeof(base_buf), "f%u", number);
        base_name = base_buf;
    } else {
        switch (base) {
            case IC_KEY_TAB:
                base_name = "tab";
                break;
            case IC_KEY_ENTER:
                base_name = "enter";
                break;
            case IC_KEY_LINEFEED:
                base_name = "linefeed";
                break;
            case IC_KEY_BACKSP:
                base_name = "backspace";
                break;
            case IC_KEY_DEL:
                base_name = "delete";
                break;
            case IC_KEY_INS:
                base_name = "insert";
                break;
            case IC_KEY_ESC:
                base_name = "esc";
                break;
            case IC_KEY_SPACE:
                base_name = "space";
                break;
            case IC_KEY_LEFT:
                base_name = "left";
                break;
            case IC_KEY_RIGHT:
                base_name = "right";
                break;
            case IC_KEY_UP:
                base_name = "up";
                break;
            case IC_KEY_DOWN:
                base_name = "down";
                break;
            case IC_KEY_HOME:
                base_name = "home";
                break;
            case IC_KEY_END:
                base_name = "end";
                break;
            case IC_KEY_PAGEUP:
                base_name = "pageup";
                break;
            case IC_KEY_PAGEDOWN:
                base_name = "pagedown";
                break;
            default:
                break;
        }
    }

    if (base_name == NULL) {
        if (base <= 0x7F && base >= 32) {
            base_buf[0] = (char)base;
            base_buf[1] = '\0';
            base_name = base_buf;
        } else if (base == IC_KEY_NONE) {
            base_name = "";
        } else {
            return false;
        }
    }

    if ((base_name[0] != '\0') && (!append_token(&first, buffer, buflen, &len, base_name))) {
        return false;
    }

    if (first) {
        return append_token(&first, buffer, buflen, &len, "none");
    }

    return true;
}

ic_public ic_key_action_t ic_key_action_from_name(const char* name) {
    if (name == NULL) {
        return IC_KEY_ACTION__MAX;
    }
    for (size_t i = 0; i < key_action_name_count(); ++i) {
        if (ic_stricmp(name, key_action_names[i].name) == 0) {
            return key_action_names[i].action;
        }
    }
    return IC_KEY_ACTION__MAX;
}

ic_public const char* ic_key_action_name(ic_key_action_t action) {
    if (action < IC_KEY_ACTION_NONE || action >= IC_KEY_ACTION__MAX) {
        return NULL;
    }
    for (size_t i = 0; i < key_action_name_count(); ++i) {
        if (key_action_names[i].action == action) {
            return key_action_names[i].name;
        }
    }
    return NULL;
}

ic_public bool ic_bind_key(ic_keycode_t key, ic_key_action_t action) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    if (action < IC_KEY_ACTION_NONE || action >= IC_KEY_ACTION__MAX) {
        return false;
    }
    ssize_t index = -1;
    ic_key_binding_entry_t* entry = key_binding_find_entry(env, key, &index);
    if (entry != NULL) {
        entry->action = action;
        return true;
    }
    if (!key_bindings_ensure_capacity(env, env->key_binding_count + 1)) {
        return false;
    }
    env->key_bindings[env->key_binding_count].key = key;
    env->key_bindings[env->key_binding_count].action = action;
    env->key_binding_count++;
    return true;
}

ic_public bool ic_clear_key_binding(ic_keycode_t key) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    ssize_t index = -1;
    if (key_binding_find_entry(env, key, &index) == NULL) {
        return false;
    }
    for (ssize_t i = index; i < env->key_binding_count - 1; ++i) {
        env->key_bindings[i] = env->key_bindings[i + 1];
    }
    if (env->key_binding_count > 0) {
        env->key_binding_count--;
    }
    return true;
}

ic_public void ic_reset_key_bindings(void) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    key_binding_clear_all(env);
    if (env->key_binding_profile != NULL) {
        (void)ic_keybinding_apply_profile(env, env->key_binding_profile);
    }
}

ic_public bool ic_get_key_binding(ic_keycode_t key, ic_key_action_t* out_action) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    ic_key_binding_entry_t* entry = key_binding_find_entry(env, key, NULL);
    if (entry == NULL) {
        return false;
    }
    if (out_action != NULL) {
        *out_action = entry->action;
    }
    return true;
}

ic_public size_t ic_list_key_bindings(ic_key_binding_entry_t* buffer, size_t capacity) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return 0;
    }
    size_t count = to_size_t(env->key_binding_count);
    if (buffer == NULL || capacity == 0) {
        return count;
    }
    size_t limit = (count < capacity ? count : capacity);
    for (size_t i = 0; i < limit; ++i) {
        buffer[i] = env->key_bindings[i];
    }
    return limit;
}

ic_public bool ic_set_key_binding_profile(const char* name) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return false;
    }
    const keybinding_profile_t* profile =
        (name == NULL ? &keybinding_profile_default : keybinding_profile_lookup(name));
    if (profile == NULL) {
        return false;
    }
    if (env->key_binding_profile == profile) {
        key_binding_clear_all(env);
        return ic_keybinding_apply_profile(env, profile);
    }
    const keybinding_profile_t* previous =
        (env->key_binding_profile != NULL ? env->key_binding_profile : &keybinding_profile_default);
    env->key_binding_profile = profile;
    key_binding_clear_all(env);
    if (!ic_keybinding_apply_profile(env, profile)) {
        env->key_binding_profile = previous;
        key_binding_clear_all(env);
        (void)ic_keybinding_apply_profile(env, env->key_binding_profile);
        return false;
    }
    return true;
}

ic_public const char* ic_get_key_binding_profile(void) {
    ic_env_t* env = ic_get_env();
    const keybinding_profile_t* profile =
        (env != NULL && env->key_binding_profile != NULL ? env->key_binding_profile
                                                         : &keybinding_profile_default);
    return profile->name;
}

ic_public size_t ic_list_key_binding_profiles(ic_key_binding_profile_info_t* buffer,
                                              size_t capacity) {
    size_t count = keybinding_profile_count();
    if (buffer == NULL || capacity == 0) {
        return count;
    }
    size_t limit = (count < capacity ? count : capacity);
    for (size_t i = 0; i < limit; ++i) {
        buffer[i].name = keybinding_profiles[i]->name;
        buffer[i].description = keybinding_profiles[i]->description;
    }
    return limit;
}

ic_public const char* ic_key_binding_profile_default_specs(ic_key_action_t action) {
    if (action <= IC_KEY_ACTION_NONE || action >= IC_KEY_ACTION__MAX) {
        return NULL;
    }
    ic_env_t* env = ic_get_env();
    const keybinding_profile_t* profile =
        (env != NULL && env->key_binding_profile != NULL ? env->key_binding_profile
                                                         : &keybinding_profile_default);
    return keybinding_profile_find_spec(profile, action);
}
