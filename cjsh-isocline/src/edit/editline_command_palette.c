/*
  editline_command_palette.c

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
// Command palette: this file is included in editline.c
//-------------------------------------------------------------

#define MAX_COMMAND_PALETTE_RESULTS 128

static const char* const k_command_palette_footer =
    "[ic-diminish](↑↓/wheel:navigate shift+↑/↓:page enter/tab:run alt+c:case "
    "ctrl+j:resize esc:cancel)[/]";

typedef struct command_palette_action_entry_s {
    ic_key_action_t action;
    const char* name;
    const char* description;
    const char* keywords;
} command_palette_action_entry_t;

typedef struct command_palette_match_s {
    bool is_custom;
    ssize_t item_idx;
    int score;
    ssize_t match_pos;
    ssize_t match_len;
} command_palette_match_t;

static const command_palette_action_entry_t command_palette_actions[] = {
    {IC_KEY_ACTION_COMPLETE, "Complete Input", "show completion suggestions at the cursor",
     "autocomplete complete suggestion finish word"},
    {IC_KEY_ACTION_HISTORY_SEARCH, "Search History", "open fuzzy history search",
     "history reverse search find previous command"},
    {IC_KEY_ACTION_HISTORY_PREV, "Previous History Entry", "load the previous command from history",
     "history back older previous"},
    {IC_KEY_ACTION_HISTORY_NEXT, "Next History Entry", "load the next command from history",
     "history forward newer next"},
    {IC_KEY_ACTION_UNDO, "Undo", "undo the latest edit", "undo revert back"},
    {IC_KEY_ACTION_REDO, "Redo", "redo the latest undone edit", "redo repeat forward"},
    {IC_KEY_ACTION_CLEAR_SCREEN, "Clear Screen", "clear the terminal screen",
     "clear cls wipe redraw"},
    {IC_KEY_ACTION_SHOW_HELP, "Show Help", "open interactive editing help",
     "help manual keybindings shortcuts"},
    {IC_KEY_ACTION_INSERT_NEWLINE, "Insert Newline", "insert a newline in multiline mode",
     "newline line break multi line"},
    {IC_KEY_ACTION_CURSOR_LEFT, "Cursor Left", "move cursor one character left",
     "move left previous character"},
    {IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, "Cursor Right", "move cursor right or complete at end",
     "move right next character smart"},
    {IC_KEY_ACTION_CURSOR_UP, "Cursor Up", "move cursor one row up", "move up row previous line"},
    {IC_KEY_ACTION_CURSOR_DOWN, "Cursor Down", "move cursor one row down",
     "move down row next line"},
    {IC_KEY_ACTION_CURSOR_LINE_START, "Line Start", "move cursor to start of line",
     "home beginning line start"},
    {IC_KEY_ACTION_CURSOR_LINE_END, "Line End", "move cursor to end of line",
     "end line finish line"},
    {IC_KEY_ACTION_CURSOR_WORD_PREV, "Previous Word", "move cursor to previous word",
     "word left backward previous token"},
    {IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, "Next Word",
     "move cursor to next word or complete at end", "word right forward next token"},
    {IC_KEY_ACTION_CURSOR_INPUT_START, "Input Start", "move cursor to start of input",
     "input beginning top first"},
    {IC_KEY_ACTION_CURSOR_INPUT_END, "Input End", "move cursor to end of input",
     "input finish bottom last"},
    {IC_KEY_ACTION_CURSOR_MATCH_BRACE, "Match Brace", "jump to matching brace",
     "brace bracket pair matching jump"},
    {IC_KEY_ACTION_DELETE_BACKWARD, "Delete Backward", "delete the character before the cursor",
     "backspace delete previous character"},
    {IC_KEY_ACTION_DELETE_FORWARD, "Delete Forward", "delete the character at the cursor",
     "delete remove next character"},
    {IC_KEY_ACTION_DELETE_WORD_END, "Delete To Word End",
     "delete from cursor to the end of the word", "kill word forward delete"},
    {IC_KEY_ACTION_DELETE_WORD_START_WS, "Delete To Whitespace",
     "delete backwards to previous whitespace", "delete whitespace backward"},
    {IC_KEY_ACTION_DELETE_WORD_START, "Delete To Word Start", "delete backwards to start of word",
     "kill word backward delete"},
    {IC_KEY_ACTION_DELETE_LINE_START, "Delete To Line Start", "delete from cursor to line start",
     "delete kill beginning line"},
    {IC_KEY_ACTION_DELETE_LINE_END, "Delete To Line End", "delete from cursor to line end",
     "delete kill end line"},
    {IC_KEY_ACTION_TRANSPOSE_CHARS, "Transpose Characters",
     "swap character with previous character", "swap transpose characters"},
    {IC_KEY_ACTION_YANK_LAST_ARG, "Insert Last Argument",
     "insert the last argument from previous history entries",
     "yank last argument previous command"},
    {IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING, "Toggle Mouse Reporting",
     "toggle mouse clicking support for this prompt", "mouse pointer click reporting toggle"},
};

static ssize_t command_palette_action_count(void) {
    return (ssize_t)(sizeof(command_palette_actions) / sizeof(command_palette_actions[0]));
}

static ssize_t command_palette_custom_count(const ic_env_t* env) {
    if (env == NULL || env->command_palette_entries == NULL ||
        env->command_palette_entry_count <= 0) {
        return 0;
    }
    return env->command_palette_entry_count;
}

static int command_palette_token_bonus(const command_palette_action_entry_t* entry,
                                       const char* query, bool case_sensitive) {
    if (entry == NULL || query == NULL || query[0] == '\0') {
        return 0;
    }

    int bonus = 0;
    const char* cursor = query;
    const char* token_start = NULL;
    size_t token_len = 0;
    while (ic_fuzzy_next_token(&cursor, &token_start, &token_len)) {
        (void)ic_fuzzy_trim_token(&token_start, &token_len);
        if (token_len < 2 || token_len >= 64) {
            continue;
        }

        char token[64];
        ic_memcpy(token, token_start, (ssize_t)token_len);
        token[token_len] = '\0';

        if (ic_fuzzy_find_substring(entry->name, token, case_sensitive, NULL, NULL)) {
            bonus += 26;
        } else if (ic_fuzzy_find_substring(entry->keywords, token, case_sensitive, NULL, NULL)) {
            bonus += 18;
        } else if (ic_fuzzy_find_substring(entry->description, token, case_sensitive, NULL, NULL)) {
            bonus += 10;
        }
    }

    return bonus;
}

static bool command_palette_score_action(const command_palette_action_entry_t* entry,
                                         const char* query, bool case_sensitive, int* score_out,
                                         ssize_t* match_pos_out, ssize_t* match_len_out) {
    if (entry == NULL || score_out == NULL || match_pos_out == NULL || match_len_out == NULL) {
        return false;
    }

    *score_out = 0;
    *match_pos_out = -1;
    *match_len_out = 0;

    if (query == NULL || query[0] == '\0') {
        return true;
    }

    ssize_t name_match_pos = -1;
    ssize_t name_match_len = 0;
    int name_score =
        ic_fuzzy_match_score(entry->name, query, &name_match_pos, &name_match_len, case_sensitive);

    ssize_t description_match_pos = -1;
    ssize_t description_match_len = 0;
    int description_score = ic_fuzzy_match_score(entry->description, query, &description_match_pos,
                                                 &description_match_len, case_sensitive);

    ssize_t keywords_match_pos = -1;
    ssize_t keywords_match_len = 0;
    int keywords_score = ic_fuzzy_match_score(entry->keywords, query, &keywords_match_pos,
                                              &keywords_match_len, case_sensitive);

    if (name_score < 0 && description_score < 0 && keywords_score < 0) {
        return false;
    }

    int best_score = -1;
    ssize_t best_match_pos = -1;
    ssize_t best_match_len = 0;

    if (name_score >= 0) {
        best_score = name_score + 90;
        best_match_pos = name_match_pos;
        best_match_len = name_match_len;
    }
    if (keywords_score >= 0) {
        int weighted = keywords_score + 55;
        if (weighted > best_score) {
            best_score = weighted;
            best_match_pos = -1;
            best_match_len = 0;
        }
    }
    if (description_score >= 0) {
        int weighted = description_score + 35;
        if (weighted > best_score) {
            best_score = weighted;
            best_match_pos = -1;
            best_match_len = 0;
        }
    }

    ssize_t exact_name_pos = -1;
    ssize_t exact_name_len = 0;
    if (ic_fuzzy_find_substring(entry->name, query, case_sensitive, &exact_name_pos,
                                &exact_name_len)) {
        best_score += 45;
        best_match_pos = exact_name_pos;
        best_match_len = exact_name_len;
    }

    best_score += command_palette_token_bonus(entry, query, case_sensitive);

    if (best_match_pos < 0 || best_match_len <= 0) {
        const char* cursor = query;
        const char* token_start = NULL;
        size_t token_len = 0;
        while (ic_fuzzy_next_token(&cursor, &token_start, &token_len)) {
            if (token_len < 2 || token_len >= 64) {
                continue;
            }

            char token[64];
            ic_memcpy(token, token_start, (ssize_t)token_len);
            token[token_len] = '\0';

            if (ic_fuzzy_find_substring(entry->name, token, case_sensitive, &best_match_pos,
                                        &best_match_len)) {
                break;
            }
        }
    }

    *score_out = best_score;
    *match_pos_out = best_match_pos;
    *match_len_out = best_match_len;
    return true;
}

static int command_palette_compare_matches(const void* left, const void* right) {
    const command_palette_match_t* a = (const command_palette_match_t*)left;
    const command_palette_match_t* b = (const command_palette_match_t*)right;

    if (a == NULL || b == NULL) {
        return 0;
    }

    if (b->score != a->score) {
        return (b->score - a->score);
    }

    if (a->is_custom != b->is_custom) {
        return (a->is_custom ? 1 : -1);
    }

    if (a->item_idx < b->item_idx) {
        return -1;
    }
    if (a->item_idx > b->item_idx) {
        return 1;
    }
    return 0;
}

static ssize_t command_palette_search_actions(ic_env_t* env, const char* query, bool case_sensitive,
                                              command_palette_match_t* matches,
                                              ssize_t max_matches) {
    if (matches == NULL || max_matches <= 0) {
        return 0;
    }

    ssize_t count = 0;
    ssize_t total_actions = command_palette_action_count();
    for (ssize_t i = 0; i < total_actions && count < max_matches; ++i) {
        const command_palette_action_entry_t* entry = &command_palette_actions[i];
        int score = 0;
        ssize_t match_pos = -1;
        ssize_t match_len = 0;

        if (!command_palette_score_action(entry, query, case_sensitive, &score, &match_pos,
                                          &match_len)) {
            continue;
        }

        if (query == NULL || query[0] == '\0') {
            score = (int)(1000 - i);
        }

        matches[count].is_custom = false;
        matches[count].item_idx = i;
        matches[count].score = score;
        matches[count].match_pos = match_pos;
        matches[count].match_len = match_len;
        count++;
    }

    ssize_t total_custom_entries = command_palette_custom_count(env);
    for (ssize_t i = 0; i < total_custom_entries && count < max_matches; ++i) {
        const ic_command_palette_entry_internal_t* custom_entry = &env->command_palette_entries[i];
        command_palette_action_entry_t score_entry = {
            IC_KEY_ACTION_NONE,
            custom_entry->name,
            custom_entry->description,
            custom_entry->keywords,
        };

        int score = 0;
        ssize_t match_pos = -1;
        ssize_t match_len = 0;

        if (!command_palette_score_action(&score_entry, query, case_sensitive, &score, &match_pos,
                                          &match_len)) {
            continue;
        }

        if (query == NULL || query[0] == '\0') {
            score = (int)(500 - i);
        }

        matches[count].is_custom = true;
        matches[count].item_idx = i;
        matches[count].score = score;
        matches[count].match_pos = match_pos;
        matches[count].match_len = match_len;
        count++;
    }

    if (count > 1) {
        qsort(matches, (size_t)count, sizeof(matches[0]), command_palette_compare_matches);
    }
    return count;
}

static void edit_command_palette(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return;
    }

    if (command_palette_action_count() + command_palette_custom_count(env) <= 0) {
        term_beep(env->term);
        return;
    }

    edit_menu_session_t menu_session =
        edit_menu_begin(env, eb, ic_env_get_command_palette_prompt(env), true);

    command_palette_match_t* matches = (command_palette_match_t*)mem_zalloc_tp_n(
        env->mem, command_palette_match_t, MAX_COMMAND_PALETTE_RESULTS);
    if (matches == NULL) {
        term_beep(env->term);
        edit_menu_finish(env, eb, &menu_session, false, false);
        return;
    }

    sbuf_clear(eb->input);
    eb->pos = 0;

    ssize_t match_count = 0;
    ssize_t selected_idx = 0;
    ssize_t scroll_offset = 0;
    ssize_t last_display_count = 0;
    ssize_t last_max_scroll = 0;
    bool session_case_sensitive = false;
    bool matches_dirty = true;
    bool showing_all_due_to_no_matches = false;
    size_t palette_generation = env->command_palette_generation;

again:;

    last_display_count = 0;
    last_max_scroll = 0;

    if (palette_generation != env->command_palette_generation) {
        palette_generation = env->command_palette_generation;
        matches_dirty = true;
    }
    if (matches_dirty) {
        showing_all_due_to_no_matches = false;
        const char* query = sbuf_string(eb->input);
        match_count = command_palette_search_actions(
            env, query ? query : "", session_case_sensitive, matches, MAX_COMMAND_PALETTE_RESULTS);

        if (match_count == 0 && query != NULL && query[0] != '\0') {
            match_count = command_palette_search_actions(env, "", session_case_sensitive, matches,
                                                         MAX_COMMAND_PALETTE_RESULTS);
            showing_all_due_to_no_matches = true;
        }
        matches_dirty = false;
    }

    if (selected_idx >= match_count) {
        selected_idx = (match_count > 0 ? match_count - 1 : 0);
    }
    if (selected_idx < 0) {
        selected_idx = 0;
    }

    sbuf_clear(eb->extra);
    const char* mouse_suffix =
        (menu_session.mouse_scroll_enabled ? " | Mouse clicking is enabled" : "");

    if (match_count > 0) {
        const char* query = sbuf_string(eb->input);
        bool is_filtered = (query != NULL && query[0] != '\0');
        if (showing_all_due_to_no_matches) {
            (void)sbuf_appendf(
                eb->extra,
                "[ic-info]No matches - showing all actions - case %s%s[/]\n",
                session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
        } else if (is_filtered) {
            (void)sbuf_appendf(eb->extra, "[ic-info]Actions found - case %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
        } else {
            (void)sbuf_appendf(eb->extra, "[ic-info]Actions - case %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
        }

        ssize_t term_width = term_get_width(env->term);
        const ssize_t reserved_rows =
            edit_menu_input_rows(env, eb) + edit_menu_rendered_rows(env, eb, sbuf_string(eb->extra)) +
            (!env->no_help ? edit_menu_rendered_rows(env, eb, k_command_palette_footer) : 0) + 1;
        ssize_t available_lines = edit_menu_available_lines(
            env, eb, reserved_rows, 1, env->command_palette_max_line_count, menu_session.maximized);
        edit_menu_window_t window =
            edit_menu_window_for(env, match_count, available_lines, selected_idx, scroll_offset);
        ssize_t display_count = window.display_count;
        scroll_offset = window.scroll_offset;

        last_display_count = display_count;
        last_max_scroll = window.max_scroll;

        for (ssize_t i = 0; i < display_count; ++i) {
            ssize_t match_idx = scroll_offset + i;
            if (match_idx >= match_count) {
                break;
            }

            const command_palette_match_t* match = &matches[match_idx];
            const char* entry_name = NULL;
            const char* entry_description = NULL;
            ic_key_action_t entry_action = IC_KEY_ACTION_NONE;
            bool entry_is_custom = match->is_custom;

            if (entry_is_custom) {
                if (match->item_idx < 0 || match->item_idx >= command_palette_custom_count(env)) {
                    continue;
                }
                const ic_command_palette_entry_internal_t* custom_entry =
                    &env->command_palette_entries[match->item_idx];
                entry_name = custom_entry->name;
                entry_description = custom_entry->description;
            } else {
                if (match->item_idx < 0 || match->item_idx >= command_palette_action_count()) {
                    continue;
                }
                const command_palette_action_entry_t* entry =
                    &command_palette_actions[match->item_idx];
                entry_name = entry->name;
                entry_description = entry->description;
                entry_action = entry->action;
            }

            if (entry_name == NULL || entry_name[0] == '\0') {
                continue;
            }
            if (entry_description == NULL) {
                entry_description = "";
            }

            char linebuf[512];
            char tag_prefix[4];
            char tagbuf[96];
            tag_prefix[0] = '\0';
            tagbuf[0] = '\0';
            int written = -1;
            if (entry_is_custom) {
                if (entry_description[0] == '\0') {
                    written = snprintf(linebuf, sizeof(linebuf), "%s", entry_name);
                    (void)snprintf(tag_prefix, sizeof(tag_prefix), " ");
                    (void)snprintf(tagbuf, sizeof(tagbuf), "(custom)");
                } else if (strstr(entry_description, "(custom)") != NULL) {
                    written = snprintf(linebuf, sizeof(linebuf), "%s", entry_name);
                    (void)snprintf(tag_prefix, sizeof(tag_prefix), " - ");
                    (void)snprintf(tagbuf, sizeof(tagbuf), "%s", entry_description);
                } else {
                    written = snprintf(linebuf, sizeof(linebuf), "%s - %s", entry_name,
                                       entry_description);
                    (void)snprintf(tag_prefix, sizeof(tag_prefix), " ");
                    (void)snprintf(tagbuf, sizeof(tagbuf), "(custom)");
                }
            } else {
                char binding_keys[64];
                format_binding_keys(env, entry_action, NULL, binding_keys, sizeof(binding_keys),
                                    true);
                if (entry_description[0] == '\0') {
                    written = snprintf(linebuf, sizeof(linebuf), "%s", entry_name);
                } else {
                    written = snprintf(linebuf, sizeof(linebuf), "%s - %s", entry_name,
                                       entry_description);
                }
                (void)snprintf(tag_prefix, sizeof(tag_prefix), " ");
                (void)snprintf(tagbuf, sizeof(tagbuf), "[%s]", binding_keys);
            }
            if (written < 0) {
                continue;
            }
            if (written >= (int)sizeof(linebuf)) {
                linebuf[sizeof(linebuf) - 1] = '\0';
            }

            const char* display = linebuf;
            const ssize_t tag_reserved_columns =
                tagbuf[0] != '\0' ? (ssize_t)(strlen(tag_prefix) + strlen(tagbuf)) : 0;
            const edit_menu_preview_t preview =
                edit_menu_preview(display, term_width - 4 - tag_reserved_columns);

            bool is_selected = (match_idx == selected_idx);
            if (is_selected) {
                (void)sbuf_append(eb->extra, "[ic-menu-selected]");
            }
            const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
            (void)sbuf_appendf(eb->extra, "[!pre]%s ", (is_selected ? arrow : " "));

            bool highlight_match = (is_filtered && !showing_all_due_to_no_matches &&
                                    match->match_len > 0 && match->match_pos >= 0);
            edit_menu_append_highlighted_prefix(
                eb->extra, display, preview.visible_len, preview.entry_len, match->match_pos,
                match->match_len, is_selected, highlight_match, NULL, false);

            if (preview.append_ellipsis) {
                (void)sbuf_append(eb->extra, "...");
            }

            (void)sbuf_append(eb->extra, "[/pre]");

            if (tagbuf[0] != '\0') {
                (void)sbuf_append(eb->extra, tag_prefix);
                edit_menu_append_tag_text(eb->extra, is_selected, tagbuf);
            }

            if (is_selected) {
                (void)sbuf_append(eb->extra, "[/ic-menu-selected]");
            }

            (void)sbuf_append(eb->extra, "\n");
        }

        edit_menu_append_scroll_hint(eb->extra, match_count, display_count, scroll_offset);
    } else {
        scroll_offset = 0;
        (void)sbuf_appendf(eb->extra, "[ic-info]No actions found - case %s%s[/]\n",
                           session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
    }

    if (!env->no_help) {
        (void)sbuf_append(eb->extra, k_command_palette_footer);
    }

    edit_refresh(env, eb);

    code_t c;
    if (!edit_menu_read_event(env, eb, &menu_session, &c)) {
        goto again;
    }
    code_t key_no_mods = KEY_NO_MODS(c);
    if (menu_session.mouse_scroll_enabled && key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        bool accept_selection = false;
        if (edit_menu_mouse_select_vertical(env, eb, match_count, scroll_offset, last_display_count,
                                            1, &selected_idx, &accept_selection)) {
            if (accept_selection) {
                c = KEY_ENTER;
            } else {
                goto again;
            }
        } else {
            if (edit_menu_mouse_event_is_left_click(env)) {
                (void)edit_menu_mouse_suspend(env, eb, &menu_session.mouse_scroll_enabled,
                                              &menu_session.mouse_suspended);
            }
            goto again;
        }
    }

    if (c == KEY_ESC || c == KEY_BELL || c == KEY_CTRL_C) {
        mem_free(env->mem, matches);
        edit_menu_finish(env, eb, &menu_session, true, true);
        return;
    }

    if (c == KEY_ENTER || c == KEY_TAB) {
        if (match_count <= 0 || selected_idx < 0 || selected_idx >= match_count) {
            term_beep(env->term);
            goto again;
        }

        const command_palette_match_t* selected_match = &matches[selected_idx];
        bool selected_is_custom = selected_match->is_custom;
        ic_key_action_t selected_action = IC_KEY_ACTION_NONE;
        ic_command_palette_entry_t selected_custom_entry = {0};

        if (selected_is_custom) {
            if (selected_match->item_idx < 0 ||
                selected_match->item_idx >= command_palette_custom_count(env)) {
                term_beep(env->term);
                goto again;
            }
            const ic_command_palette_entry_internal_t* custom_entry =
                &env->command_palette_entries[selected_match->item_idx];
            selected_custom_entry.id = custom_entry->id;
            selected_custom_entry.name = custom_entry->name;
            selected_custom_entry.description = custom_entry->description;
            selected_custom_entry.keywords = custom_entry->keywords;
        } else {
            if (selected_match->item_idx < 0 ||
                selected_match->item_idx >= command_palette_action_count()) {
                term_beep(env->term);
                goto again;
            }
            selected_action = command_palette_actions[selected_match->item_idx].action;
        }

        mem_free(env->mem, matches);
        // Clear the palette before an action can suspend or replace the editor display.
        edit_menu_finish(env, eb, &menu_session, true, true);

        bool handled = false;
        if (selected_is_custom) {
            if (env->command_palette_handler != NULL) {
                handled = env->command_palette_handler(&selected_custom_entry,
                                                       env->command_palette_handler_arg);
            }
        } else {
            handled = key_action_execute(env, eb, selected_action, KEY_NONE);
        }

        if (!handled) {
            term_beep(env->term);
        }
        edit_refresh(env, eb);
        return;
    }

    const edit_menu_input_t change = edit_menu_handle_input(
        env, eb, c, &menu_session, match_count, last_display_count, last_max_scroll, &scroll_offset,
        &selected_idx, &session_case_sensitive, false);
    if (change == EDIT_MENU_INPUT_QUERY) {
        selected_idx = 0;
    }
    if (change == EDIT_MENU_INPUT_QUERY || change == EDIT_MENU_INPUT_CASE) {
        matches_dirty = true;
    } else if (change == EDIT_MENU_INPUT_UNHANDLED) {
        term_beep(env->term);
    }
    goto again;
}
