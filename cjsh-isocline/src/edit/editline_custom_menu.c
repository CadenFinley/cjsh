/*
  editline_custom_menu.c

  This file is part of isocline

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

//-------------------------------------------------------------
// Application-provided menu: this file is included in editline.c
//-------------------------------------------------------------

static const char* const k_custom_menu_footer =
    "[ic-diminish](↑↓/wheel:navigate shift+↑/↓:page enter/tab:select "
    "alt+c:case ctrl+j:resize esc:cancel)[/]";

typedef struct custom_menu_match_s {
    ssize_t item_idx;
    int score;
    ssize_t match_pos;
    ssize_t match_len;
} custom_menu_match_t;

static int custom_menu_compare_matches(const void* left, const void* right) {
    const custom_menu_match_t* a = (const custom_menu_match_t*)left;
    const custom_menu_match_t* b = (const custom_menu_match_t*)right;
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (b->score != a->score) {
        return (b->score - a->score);
    }
    if (a->item_idx < b->item_idx) {
        return -1;
    }
    if (a->item_idx > b->item_idx) {
        return 1;
    }
    return 0;
}

static ssize_t custom_menu_search(const ic_menu_item_t* items, ssize_t item_count,
                                  const char* query, bool case_sensitive,
                                  custom_menu_match_t* matches) {
    if (items == NULL || item_count <= 0 || matches == NULL) {
        return 0;
    }

    ssize_t count = 0;
    for (ssize_t i = 0; i < item_count; ++i) {
        const ic_menu_item_t* item = &items[i];
        command_palette_action_entry_t score_entry = {
            IC_KEY_ACTION_NONE,
            item->label,
            (item->description != NULL ? item->description : ""),
            (item->keywords != NULL ? item->keywords : ""),
        };
        int score = 0;
        ssize_t match_pos = -1;
        ssize_t match_len = 0;
        if (!command_palette_score_action(&score_entry, query, case_sensitive, &score, &match_pos,
                                          &match_len)) {
            continue;
        }
        matches[count].item_idx = i;
        matches[count].score = score;
        matches[count].match_pos = match_pos;
        matches[count].match_len = match_len;
        count++;
    }

    if (count > 1 && query != NULL && query[0] != '\0') {
        qsort(matches, (size_t)count, sizeof(matches[0]), custom_menu_compare_matches);
    }
    return count;
}

static void custom_menu_render_item(ic_env_t* env, editor_t* eb, stringbuf_t* display_buffer,
                                    const ic_menu_item_t* item, const custom_menu_match_t* match,
                                    bool is_filtered, bool is_selected,
                                    ssize_t selected_preview_limit) {
    if (env == NULL || eb == NULL || display_buffer == NULL || item == NULL || match == NULL) {
        return;
    }

    sbuf_clear(display_buffer);
    (void)sbuf_append(display_buffer, item->label);
    if (item->description != NULL && item->description[0] != '\0') {
        (void)sbuf_append(display_buffer, " - ");
        (void)sbuf_append(display_buffer, item->description);
    }

    const char* display = sbuf_string(display_buffer);
    if (display == NULL) {
        return;
    }

    // Keep ordinary entries compact, but let the focused item's description use the same
    // multi-row preview treatment as history search. This makes long descriptions readable
    // without allowing every entry to consume the menu viewport.
    if (is_selected && item->description != NULL && item->description[0] != '\0' &&
        selected_preview_limit > 1) {
        (void)edit_menu_append_multiline_preview(env, eb, display, false, false,
                                                 selected_preview_limit, NULL, NULL);
        (void)sbuf_append(eb->extra, "\n");
        return;
    }
    const edit_menu_preview_t preview = edit_menu_preview(display, edit_menu_content_width(env) - 2);

    if (is_selected) {
        (void)sbuf_append(eb->extra, "[ic-menu-selected]");
    }
    const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
    (void)sbuf_appendf(eb->extra, "[!pre]%s ", (is_selected ? arrow : " "));
    bool highlight_match = (is_filtered && match->match_len > 0 && match->match_pos >= 0);
    edit_menu_append_highlighted_prefix(eb->extra, display, preview.visible_len, preview.entry_len,
                                        match->match_pos, match->match_len, is_selected,
                                        highlight_match, NULL, false);
    if (preview.append_ellipsis) {
        (void)sbuf_append(eb->extra, "...");
    }
    (void)sbuf_append(eb->extra, "[/pre]");
    if (is_selected) {
        (void)sbuf_append(eb->extra, "[/ic-menu-selected]");
    }
    (void)sbuf_append(eb->extra, "\n");
}

static bool custom_menu_mouse_select(ic_env_t* env, editor_t* eb, ssize_t item_count,
                                     ssize_t scroll_offset, ssize_t display_count,
                                     ssize_t selected_idx, ssize_t selected_preview_limit,
                                     ssize_t* new_selected_idx, bool* accept_selection) {
    if (env == NULL || eb == NULL || env->tty == NULL || new_selected_idx == NULL ||
        accept_selection == NULL) {
        return false;
    }

    *accept_selection = false;
    tty_mouse_event_t mouse_event;
    if (!tty_get_last_mouse_event(env->tty, &mouse_event) ||
        (mouse_event.action != TTY_MOUSE_ACTION_LEFT_PRESS &&
         mouse_event.action != TTY_MOUSE_ACTION_LEFT_RELEASE)) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &mouse_event, &target_row, &target_col, NULL)) {
        return false;
    }
    ic_unused(target_col);

    const ssize_t input_rows = (eb->input_rows > 0 ? eb->input_rows : 1);
    ssize_t item_row = target_row - (input_rows + 1);  // one status row precedes menu items
    if (item_row < 0) {
        return false;
    }

    for (ssize_t visible_idx = 0; visible_idx < display_count; ++visible_idx) {
        const ssize_t idx = scroll_offset + visible_idx;
        if (idx >= item_count) {
            break;
        }
        const ssize_t item_rows =
            (idx == selected_idx && selected_preview_limit > 1 ? selected_preview_limit : 1);
        if (item_row < item_rows) {
            *new_selected_idx = idx;
            *accept_selection = (mouse_event.action == TTY_MOUSE_ACTION_LEFT_RELEASE);
            return true;
        }
        item_row -= item_rows;
    }
    return false;
}

static bool edit_custom_menu(ic_env_t* env, editor_t* eb, const char* prompt_text,
                             const ic_menu_item_t* items, ssize_t item_count,
                             size_t* selected_index, ic_menu_accept_t* accept) {
    if (env == NULL || eb == NULL || env->tty == NULL || items == NULL || item_count <= 0 ||
        item_count > SSIZE_MAX / ssizeof(custom_menu_match_t)) {
        return false;
    }

    custom_menu_match_t* matches = mem_zalloc_tp_n(env->mem, custom_menu_match_t, item_count);
    stringbuf_t* display_buffer = sbuf_new(env->mem);
    if (matches == NULL || display_buffer == NULL) {
        mem_free(env->mem, matches);
        sbuf_free(display_buffer);
        return false;
    }

    edit_menu_session_t menu_session = edit_menu_begin(env, eb, prompt_text, true);
    sbuf_clear(eb->input);
    eb->pos = 0;

    ssize_t match_count = 0;
    ssize_t selected_idx = 0;
    ssize_t scroll_offset = 0;
    ssize_t last_display_count = 0;
    ssize_t last_max_scroll = 0;
    ssize_t selected_preview_limit = 0;
    bool session_case_sensitive = false;
    bool matches_dirty = true;
    ic_menu_accept_t accepted = IC_MENU_ACCEPT_NONE;
    bool accepted_with_mouse = false;

again:;

    const char* query = sbuf_string(eb->input);
    bool is_filtered = (query != NULL && query[0] != '\0');
    if (matches_dirty) {
        match_count = custom_menu_search(items, item_count, query, session_case_sensitive, matches);
        matches_dirty = false;
    }
    if (selected_idx >= match_count) {
        selected_idx = (match_count > 0 ? match_count - 1 : 0);
    }
    if (selected_idx < 0) {
        selected_idx = 0;
    }

    last_display_count = 0;
    last_max_scroll = 0;
    selected_preview_limit = 0;
    menu_session.scrollbar.rows = 0;
    sbuf_clear(eb->extra);
    const char* mouse_suffix =
        (menu_session.mouse_scroll_enabled ? " | Mouse clicking is enabled" : "");

    if (match_count > 0) {
        if (is_filtered) {
            (void)sbuf_appendf(eb->extra, "[ic-info]Items found - case %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
        } else {
            (void)sbuf_appendf(eb->extra, "[ic-info]Items - case %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
        }

        if (selected_idx >= 0 && selected_idx < match_count) {
            const ic_menu_item_t* selected_item = &items[matches[selected_idx].item_idx];
            if (selected_item->description != NULL && selected_item->description[0] != '\0') {
                sbuf_clear(display_buffer);
                (void)sbuf_append(display_buffer, selected_item->label);
                (void)sbuf_append(display_buffer, " - ");
                (void)sbuf_append(display_buffer, selected_item->description);
                selected_preview_limit =
                    edit_menu_multiline_preview_row_count(env, sbuf_string(display_buffer));
            }
        }

        const ssize_t reserved_rows =
            edit_menu_input_rows(env, eb) + edit_menu_rendered_rows(env, eb, sbuf_string(eb->extra)) +
            (!env->no_help ? edit_menu_rendered_rows(env, eb, k_custom_menu_footer) : 0) + 1;
        ssize_t available_lines = edit_menu_available_lines(
            env, eb, reserved_rows, 1, env->custom_menu_max_line_count, menu_session.maximized);
        if (selected_preview_limit > available_lines) {
            selected_preview_limit = available_lines;
        }
        ssize_t rows_for_items = available_lines;
        if (selected_preview_limit > 1) {
            rows_for_items -= selected_preview_limit - 1;
            if (rows_for_items < 1) {
                rows_for_items = 1;
            }
        }
        edit_menu_window_t window =
            edit_menu_window_for(env, match_count, rows_for_items, selected_idx, scroll_offset);
        last_display_count = window.display_count;
        last_max_scroll = window.max_scroll;
        scroll_offset = window.scroll_offset;

        const ssize_t items_start = sbuf_len(eb->extra);
        for (ssize_t i = 0; i < last_display_count; ++i) {
            ssize_t match_idx = scroll_offset + i;
            if (match_idx >= match_count) {
                break;
            }
            const custom_menu_match_t* match = &matches[match_idx];
            if (match->item_idx < 0 || match->item_idx >= item_count) {
                continue;
            }
            custom_menu_render_item(env, eb, display_buffer, &items[match->item_idx], match,
                                    is_filtered, match_idx == selected_idx, selected_preview_limit);
        }
        edit_menu_append_scrollbar(env, eb, &menu_session.scrollbar, items_start, &window);
        edit_menu_append_scroll_hint(eb->extra, match_count, last_display_count, scroll_offset);
    } else {
        scroll_offset = 0;
        (void)sbuf_appendf(eb->extra, "[ic-info]No items found - case %s%s[/]\n",
                           session_case_sensitive ? "sensitive" : "insensitive", mouse_suffix);
    }

    if (!env->no_help) {
        (void)sbuf_append(eb->extra, k_custom_menu_footer);
    }
    edit_refresh(env, eb);

    accepted_with_mouse = false;
    code_t c;
    if (!edit_menu_read_event(env, eb, &menu_session, &c, &scroll_offset, &selected_idx)) {
        goto again;
    }
    code_t key_no_mods = KEY_NO_MODS(c);
    if (menu_session.mouse_scroll_enabled && key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        bool accept_selection = false;
        if (custom_menu_mouse_select(env, eb, match_count, scroll_offset, last_display_count,
                                     selected_idx, selected_preview_limit, &selected_idx,
                                     &accept_selection)) {
            if (accept_selection) {
                accepted_with_mouse = true;
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
        goto done;
    }
    if (c == KEY_ENTER || c == KEY_TAB) {
        if (match_count <= 0 || selected_idx < 0 || selected_idx >= match_count) {
            term_beep(env->term);
            goto again;
        }
        ssize_t item_idx = matches[selected_idx].item_idx;
        if (item_idx < 0 || item_idx >= item_count) {
            term_beep(env->term);
            goto again;
        }
        if (selected_index != NULL) {
            *selected_index = to_size_t(item_idx);
        }
        accepted = (c == KEY_ENTER && !accepted_with_mouse ? IC_MENU_ACCEPT_SUBMIT
                                                           : IC_MENU_ACCEPT_INSERT);
        goto done;
    }

    const edit_menu_input_t change = edit_menu_handle_input(
        env, eb, c, &menu_session, match_count, last_display_count, last_max_scroll, &scroll_offset,
        &selected_idx, &session_case_sensitive, false);
    if (change == EDIT_MENU_INPUT_QUERY || change == EDIT_MENU_INPUT_CASE) {
        matches_dirty = true;
        selected_idx = 0;
        scroll_offset = 0;
    }
    goto again;

done:
    sbuf_free(display_buffer);
    mem_free(env->mem, matches);
    edit_menu_finish(env, eb, &menu_session, true, true);
    if (accept != NULL) {
        *accept = accepted;
    }
    return accepted != IC_MENU_ACCEPT_NONE;
}
