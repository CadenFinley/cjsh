/*
  editline_completion.c

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

// Completion menu: this file is included in editline.c
//-------------------------------------------------------------

#define IC_LARGE_MENU_SOURCE_LIMIT 70

static bool edit_completion_commit(editor_t* eb, ssize_t newpos) {
    if (newpos == IC_COMP_APPLY_FAIL) {
        editor_undo_restore(eb, false);
        return false;
    }
    if (newpos == IC_COMP_APPLY_NOOP) {
        editor_undo_forget(eb);
        return false;
    }
    eb->pos = newpos;
    return true;
}

// return true if anything changed
static bool edit_complete(ic_env_t* env, editor_t* eb, ssize_t idx) {
    editor_start_modify(eb);
    ssize_t newpos = completions_apply(env->completions, idx, eb->input, eb->pos);
    bool changed = edit_completion_commit(eb, newpos);

    if (changed) {
        (void)edit_expand_abbreviation_if_needed(env, eb, true);
        if (!env->completion_auto_menu || !eb->completion_menu_active) {
            edit_refresh(env, eb);
        }
    } else if (newpos == IC_COMP_APPLY_NOOP && completions_count(env->completions) > 1 &&
               (!env->completion_auto_menu || !eb->completion_menu_active)) {
        edit_refresh(env, eb);
    }
    return changed;
}

static void edit_complete_longest_prefix(ic_env_t* env, editor_t* eb) {
    editor_start_modify(eb);
    ssize_t newpos = completions_apply_longest_prefix(env->completions, eb->input, eb->pos);
    if (!edit_completion_commit(eb, newpos)) {
        return;
    }
    (void)edit_expand_abbreviation_if_needed(env, eb, true);
    edit_refresh(env, eb);
}

ic_private void sbuf_append_tagged(stringbuf_t* sb, const char* tag, const char* content) {
    (void)sbuf_appendf(sb, "[%s]", tag);
    (void)sbuf_append(sb, content);
    (void)sbuf_append(sb, "[/]");
}

static const char* completion_single_line_view(alloc_t* mem, const char* display,
                                               char** allocated) {
    if (allocated != NULL) {
        *allocated = NULL;
    }
    if (display == NULL) {
        return "";
    }
    const char* line_end = edit_menu_first_line_end(display);
    if (line_end == NULL || *line_end == '\0') {
        return display;
    }
    size_t prefix_len = (size_t)(line_end - display);
    size_t truncated_len = prefix_len + 3;  // account for "..."
    char* truncated = mem_malloc_tp_n(mem, char, (ssize_t)(truncated_len + 1));
    if (truncated == NULL) {
        return display;
    }
    if (prefix_len > 0) {
        memcpy(truncated, display, prefix_len);
    }
    memcpy(truncated + prefix_len, "...", 3);
    truncated[truncated_len] = '\0';
    if (allocated != NULL) {
        *allocated = truncated;
    }
    return truncated;
}

static const char* completion_source_view(alloc_t* mem, const char* source, ssize_t max_chars,
                                          char** allocated) {
    if (allocated != NULL) {
        *allocated = NULL;
    }
    if (source == NULL) {
        return NULL;
    }
    const char* line_end = edit_menu_first_line_end(source);
    ssize_t len = (line_end == NULL ? ic_strlen(source) : (ssize_t)(line_end - source));
    bool multiline = (line_end != NULL && (*line_end == '\n' || *line_end == '\r'));
    if (!multiline && (max_chars <= 0 || len <= max_chars)) {
        return source;
    }

    ssize_t ellipsis = (max_chars <= 0 || max_chars >= 3 ? 3 : 0);
    ssize_t copy_len = len;
    if (max_chars > 0) {
        ssize_t max_copy_len = max_chars - ellipsis;
        if (max_copy_len < 0) {
            max_copy_len = 0;
        }
        if (copy_len > max_copy_len) {
            copy_len = max_copy_len;
        }
    }
    ssize_t total = copy_len + ellipsis;
    char* truncated = mem_malloc_tp_n(mem, char, total + 1);
    if (truncated == NULL) {
        return source;
    }
    if (copy_len > 0) {
        memcpy(truncated, source, (size_t)copy_len);
    }
    if (ellipsis > 0) {
        memcpy(truncated + copy_len, "...", (size_t)ellipsis);
    }
    truncated[total] = '\0';
    if (allocated != NULL) {
        *allocated = truncated;
    }
    return truncated;
}

static void editor_append_completion(ic_env_t* env, editor_t* eb, ssize_t idx, ssize_t width,
                                     bool selected) {
    const char* help = NULL;
    const char* display = completions_get_display(env->completions, idx, &help);
    const char* source = completions_get_source(env->completions, idx);
    if (display == NULL) {
        return;
    }

    const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
    ssize_t width_remaining = width;
    const char* source_style = edit_menu_tag_style(selected);
    const char* help_style = (selected ? "ic-menu-selected-secondary" : "ic-info");

    if (selected) {
        (void)sbuf_append(eb->extra, "[ic-menu-selected]");
    }

    ssize_t prefix_width = 2;
    width_remaining -= prefix_width;
    (void)sbuf_appendf(eb->extra, "%s ", (selected ? arrow : " "));

    // Menu sizing and mouse hit testing both rely on one physical row per item.
    bool apply_width_constraint = (width_remaining > 0);
    if (apply_width_constraint) {
        (void)sbuf_appendf(eb->extra, "[width=\"%zd;left; ;on\"]", width_remaining);
    }
    char* single_line_alloc = NULL;
    const char* single_line_display =
        completion_single_line_view(env->mem, display, &single_line_alloc);
    if (edit_menu_should_syntax_highlight_item(env, selected)) {
        if (!edit_menu_append_completion_syntax_highlighted_text(
                env, eb, eb->extra, env->completions, idx, single_line_display, -1, true, -1, 0,
                selected, false, NULL)) {
            edit_menu_append_syntax_highlighted_text(env, eb->extra, single_line_display, -1, true,
                                                     -1, 0, selected, false, NULL);
        }
    } else {
        (void)sbuf_append(eb->extra, single_line_display);
    }
    if (single_line_alloc != NULL) {
        mem_free(env->mem, single_line_alloc);
    }

    // Add source information if available
    const char* source_display = source;
    char* source_alloc = NULL;
    if (source != NULL) {
        ssize_t limit = IC_LARGE_MENU_SOURCE_LIMIT;
        source_display = completion_source_view(env->mem, source, limit, &source_alloc);
    }
    if (source_display != NULL) {
        (void)sbuf_append(eb->extra, " ");
        sbuf_append_tagged(eb->extra, source_style, "(");
        sbuf_append_tagged(eb->extra, source_style, source_display);
        sbuf_append_tagged(eb->extra, source_style, ")");
    }

    if (help != NULL) {
        char* help_alloc = NULL;
        const char* help_display = completion_single_line_view(env->mem, help, &help_alloc);
        (void)sbuf_append(eb->extra, "  ");
        sbuf_append_tagged(eb->extra, help_style, help_display);
        mem_free(env->mem, help_alloc);
    }
    if (apply_width_constraint) {
        (void)sbuf_append(eb->extra, "[/width]");
    }
    if (selected) {
        (void)sbuf_append(eb->extra, "[/ic-menu-selected]");
    }
    if (source_alloc != NULL) {
        mem_free(env->mem, source_alloc);
    }
}

static ssize_t edit_completions_max_width(ic_env_t* env, ssize_t count, ssize_t source_limit) {
    ssize_t max_width = 0;
    for (ssize_t i = 0; i < count; i++) {
        const char* help = NULL;
        const char* source = completions_get_source(env->completions, i);
        const char* display = completions_get_display(env->completions, i, &help);
        char* display_alloc = NULL;
        const char* single_line_display =
            completion_single_line_view(env->mem, display, &display_alloc);
        ssize_t w = bbcode_column_width(env->bbcode, single_line_display);

        // Add space for source information if available
        if (source != NULL) {
            char* source_alloc = NULL;
            const char* limited_source =
                completion_source_view(env->mem, source, source_limit, &source_alloc);
            if (limited_source != NULL) {
                w += 3 + bbcode_column_width(env->bbcode, limited_source);
            }
            if (source_alloc != NULL) {
                mem_free(env->mem, source_alloc);
            }
        }

        if (help != NULL) {
            w += 2 + bbcode_column_width(env->bbcode, help);
        }
        if (w > max_width) {
            max_width = w;
        }
        if (display_alloc != NULL) {
            mem_free(env->mem, display_alloc);
        }
    }
    return max_width;
}

static bool edit_completion_auto_menu_has_prefix(editor_t* eb) {
    // Wait for input in the next argument, including when the cursor moves between words.
    return !edit_current_line_is_empty(eb) && eb->pos > 0 &&
           !ic_char_is_white(sbuf_string(eb->input) + eb->pos - 1, 1);
}

// A passive menu is only rendered here: it never reads keys, captures the mouse, applies a
// common prefix, or previews a replacement. The main editor continues to own all input.
static void edit_refresh_completion_auto_menu(ic_env_t* env, editor_t* eb) {
    sbuf_clear(eb->extra);
    eb->completion_auto_menu_visible = false;
    if (!edit_completion_auto_menu_has_prefix(eb)) {
        eb->completion_menu_maximized = false;
        edit_refresh(env, eb);
        return;
    }

    // Passive suggestions run on every edit. Use hint semantics to let completers reuse
    // caches and defer process launches until Tab, while retaining the full menu budget.
    const ssize_t count = completions_generate_hint(env, env->completions, sbuf_string(eb->input),
                                                    eb->pos, IC_MAX_COMPLETIONS_TO_TRY);
    if (count <= 0) {
        eb->completion_menu_maximized = false;
        edit_refresh(env, eb);
        return;
    }
    completions_sort(env->completions);

    char footer[192];
    (void)snprintf(footer, sizeof(footer),
                   "[ic-diminish](tab:%s up/down:activate right:accept%s ctrl+j:resize esc:hide)[/]",
                   (count == 1 ? "complete" : "activate completions"),
                   (eb->mouse_reporting_enabled ? " wheel/click:activate" : ""));
    const char* more = (count >= IC_MAX_COMPLETIONS_TO_TRY ? " (more available)" : "");
    char header[192];
    (void)snprintf(header, sizeof(header), "[ic-info]Completions%s[/]\n", more);
    const ssize_t reserved_rows = edit_menu_input_rows(env, eb) +
                                  edit_menu_rendered_rows(env, eb, header) +
                                  edit_menu_rendered_rows(env, eb, footer) + 1;
    const ssize_t available =
        edit_menu_available_lines(env, eb, reserved_rows, 1, env->completion_menu_max_line_count,
                                  eb->completion_menu_maximized);
    const edit_menu_window_t window = edit_menu_window_for(env, count, available, -1, 0);
    const ssize_t visible = window.display_count;
    ssize_t width = edit_completions_max_width(env, count, IC_LARGE_MENU_SOURCE_LIMIT) + 6;
    const ssize_t max_width = edit_menu_content_width(env) - 1;
    if (max_width > 0 && width > max_width) {
        width = max_width;
    }

    eb->completion_auto_menu_header_rows = edit_menu_rendered_rows(env, eb, header);
    eb->completion_auto_menu_item_rows = visible;
    (void)sbuf_append(eb->extra, header);
    const ssize_t items_start = sbuf_len(eb->extra);
    for (ssize_t idx = 0; idx < visible; idx++) {
        editor_append_completion(env, eb, idx, width, false);
        (void)sbuf_append(eb->extra, "\n");
    }
    edit_menu_scrollbar_t scrollbar = {0};
    edit_menu_append_scrollbar(env, eb, &scrollbar, items_start, &window);
    edit_menu_append_scroll_hint(eb->extra, count, visible, window.scroll_offset);
    (void)sbuf_append(eb->extra, footer);
    eb->completion_auto_menu_rows = edit_menu_rendered_rows(env, eb, sbuf_string(eb->extra));
    eb->completion_auto_menu_visible = true;
    edit_refresh(env, eb);
}

static void edit_completion_menu_update_hint(ic_env_t* env, editor_t* eb, bool allow_inline_hint) {
    sbuf_clear(eb->hint);
    sbuf_clear(eb->hint_help);

    if (env->no_hint || edit_current_line_is_empty(eb)) {
        return;
    }

    ssize_t hint_count = completions_count(env->completions);
    if (hint_count <= 0) {
        return;
    }

    const char* help = NULL;
    const char* hint = completions_get_hint(env->completions, 0, &help);
    if (hint == NULL || *hint == '\0') {
        return;
    }

    if (allow_inline_hint) {
        sbuf_replace(eb->hint, hint);
    }
    if (help != NULL) {
        editor_append_hint_help(eb, help);
    }
}

static ssize_t edit_completion_preview_input_rows(ic_env_t* env, editor_t* eb, ssize_t selected,
                                                  ssize_t reserved_rows, ssize_t* preview_len) {
    *preview_len = -1;
    ssize_t current_rows = edit_menu_input_rows(env, eb);
    if (env == NULL || eb == NULL || env->complete_nopreview || selected < 0 ||
        env->completions == NULL || eb->input == NULL) {
        return current_rows;
    }

    const char* input = sbuf_string(eb->input);
    if (input == NULL || eb->pos < 0) {
        return current_rows;
    }

    const char* replacement = NULL;
    ssize_t replacement_start = 0;
    ssize_t delete_after = 0;
    if (!completions_get_apply_range(env->completions, selected, input, eb->pos, &replacement,
                                     &replacement_start, &delete_after) ||
        replacement == NULL) {
        return current_rows;
    }

    const ssize_t input_len = ic_strlen(input);
    if (input_len < 0 || eb->pos > input_len) {
        return current_rows;
    }

    if (replacement_start < 0) {
        replacement_start = 0;
    }
    if (replacement_start > input_len) {
        replacement_start = input_len;
    }

    ssize_t suffix_start = eb->pos + delete_after;
    if (suffix_start < 0) {
        suffix_start = 0;
    }
    if (suffix_start > input_len) {
        suffix_start = input_len;
    }

    stringbuf_t* preview = sbuf_new(env->mem);
    if (preview == NULL) {
        return current_rows;
    }

    (void)sbuf_append_n(preview, input, replacement_start);
    (void)sbuf_append(preview, replacement);
    if (suffix_start < input_len) {
        (void)sbuf_append_n(preview, input + suffix_start, input_len - suffix_start);
    }

    ssize_t promptw = 0;
    ssize_t cpromptw = 0;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);

    rowcol_t rc_dummy;
    memset(&rc_dummy, 0, sizeof(rc_dummy));
    ssize_t preview_rows =
        sbuf_get_rc_at_pos(preview, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                           sbuf_len(preview), &rc_dummy);

    ssize_t max_preview_rows = edit_available_terminal_rows(env, eb) - reserved_rows;
    if (max_preview_rows < 1) {
        max_preview_rows = 1;
    }
    max_preview_rows = edit_visible_input_row_count(env, eb, max_preview_rows);
    if (preview_rows > max_preview_rows) {
        // Keep the beginning (and prompt) visible instead of scrolling to the end of a tall
        // replacement. Only the temporary preview is shortened; acceptance applies the full text.
        // Leave room for the dots and cursor so the shortened preview stays on this row.
        ssize_t last_columns = eb->termw - (max_preview_rows == 1 ? promptw : cpromptw) - 4;
        if (last_columns < 0) {
            last_columns = 0;
        }
        ssize_t visible_len =
            sbuf_get_pos_at_rc(preview, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                               max_preview_rows - 1, last_columns);
        if (visible_len < 0) {
            visible_len = 0;
        }
        rowcol_t visible_rc = {0};
        (void)sbuf_get_rc_at_pos(preview, eb->termw, promptw, cpromptw, env->line_wrap_marker_width,
                                 visible_len, &visible_rc);
        if (visible_len > 0 && visible_rc.col > last_columns) {
            // A wide character can straddle the requested column.
            visible_len = sbuf_prev(preview, visible_len, NULL);
        }
        while (visible_len > 0 && (sbuf_char_at(preview, visible_len - 1) == '\n' ||
                                   sbuf_char_at(preview, visible_len - 1) == '\r')) {
            visible_len--;
        }
        *preview_len = visible_len;
        preview_rows = max_preview_rows;
    }
    sbuf_free(preview);

    if (preview_rows <= 0) {
        preview_rows = 1;
    }
    return edit_visible_input_row_count(env, eb, preview_rows);
}

static const char* edit_completion_menu_footer(bool more_available) {
    if (more_available) {
        return "[ic-diminish](↑↓/tab/wheel:move shift+↑/↓:page enter/right:accept "
               "pgdn:load ctrl+j:resize esc:cancel)[/]";
    }
    return "[ic-diminish](↑↓/tab/wheel:move shift+↑/↓:page enter/right:accept "
           "pgup/pgdn:page ctrl+j:resize esc:cancel)[/]";
}

static bool completion_menu_mouse_select(ic_env_t* env, editor_t* eb, ssize_t scroll_offset,
                                         ssize_t count_displayed, ssize_t last_rows_visible,
                                         ssize_t status_rows, ssize_t* selected,
                                         bool* accept_selection) {
    if (env == NULL || eb == NULL || env->tty == NULL || selected == NULL ||
        accept_selection == NULL) {
        return false;
    }

    *accept_selection = false;

    tty_mouse_event_t mouse_event;
    if (!tty_get_last_mouse_event(env->tty, &mouse_event)) {
        return false;
    }

    if (mouse_event.action != TTY_MOUSE_ACTION_LEFT_PRESS &&
        mouse_event.action != TTY_MOUSE_ACTION_LEFT_RELEASE) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &mouse_event, &target_row, &target_col, NULL)) {
        return false;
    }

    const ssize_t input_rows = (eb->input_rows > 0 ? eb->input_rows : 1);
    const ssize_t items_first_row = input_rows + status_rows;
    const ssize_t item_row = target_row - items_first_row;
    if (item_row < 0) {
        return false;
    }

    const ssize_t visible_rows = (last_rows_visible > 0 ? last_rows_visible : count_displayed);
    if (item_row >= visible_rows) {
        return false;
    }
    const ssize_t idx = scroll_offset + item_row;

    if (idx < 0 || idx >= count_displayed) {
        return false;
    }

    *selected = idx;
    *accept_selection = (mouse_event.action == TTY_MOUSE_ACTION_LEFT_RELEASE);
    return true;
}

static bool edit_recompute_completion_list(ic_env_t* env, editor_t* eb, ssize_t* count,
                                           bool* more_available, ssize_t* selected,
                                           ssize_t* scroll_offset, bool allow_inline_hint) {
    const ssize_t limit = IC_MAX_COMPLETIONS_TO_SHOW;
    // Editing an active menu back to an empty argument hands input back to the editor.
    ssize_t new_count = 0;
    if (!env->completion_auto_menu || edit_completion_auto_menu_has_prefix(eb)) {
        new_count =
            completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos, limit);
    }
    bool new_more_available = (new_count >= limit);

    if (new_count <= 0) {
        completions_clear(env->completions);
        sbuf_clear(eb->hint);
        sbuf_clear(eb->hint_help);
        return false;
    }

    completions_sort(env->completions);
    *count = new_count;
    *more_available = new_more_available;

    if (*selected >= new_count) {
        *selected = (new_count > 0 ? new_count - 1 : -1);
    }
    if (env->complete_nopreview && *selected < 0 && new_count > 0) {
        *selected = 0;
    }

    if (scroll_offset != NULL) {
        *scroll_offset = 0;
    }

    edit_completion_menu_update_hint(env, eb, allow_inline_hint);

    return true;
}

static void edit_completion_menu(ic_env_t* env, editor_t* eb, bool more_available,
                                 ssize_t selected) {
    ssize_t count = completions_count(env->completions);
    if (count <= 0) {
        sbuf_clear(eb->extra);
        sbuf_clear(eb->hint);
        sbuf_clear(eb->hint_help);
        edit_refresh(env, eb);
        completions_clear(env->completions);
        return;
    }
    eb->completion_menu_active = true;
    eb->completion_auto_menu_visible = false;
    bool menu_mouse_scroll_enabled = false;
    bool menu_mouse_suspended = false;
    bool menu_mouse_focus_reporting_added = false;
    edit_menu_scrollbar_t scrollbar = {0};
    bool completion_applied = false;
    bool completion_accepted = false;
    const bool hints_enabled = !env->no_hint && !env->completion_auto_menu;
    char* saved_input = NULL;
    char* saved_hint = NULL;
    char* saved_hint_help = NULL;
    ssize_t saved_pos = eb->pos;
    if (hints_enabled) {
        saved_input = sbuf_strdup(eb->input);
        if (sbuf_len(eb->hint) > 0) {
            saved_hint = sbuf_strdup(eb->hint);
        }
        if (sbuf_len(eb->hint_help) > 0) {
            saved_hint_help = sbuf_strdup(eb->hint_help);
        }
    }

    sbuf_clear(eb->hint);
    sbuf_clear(eb->hint_help);
    edit_completion_menu_update_hint(env, eb, false);
    if (selected < 0 || selected >= count) {
        selected = 0;
    }
    ssize_t scroll_offset = 0;
    ssize_t last_rows_visible = 0;
    ssize_t last_max_scroll_offset = 0;
    ssize_t last_header_rows = 1;
    ssize_t count_displayed = count;
    code_t c = 0;

again:
    sbuf_clear(eb->extra);
    scrollbar.rows = 0;
    last_rows_visible = 0;
    last_max_scroll_offset = 0;
    last_header_rows = 1;

    if (count <= 0) {
        edit_refresh(env, eb);
        goto read_key;
    }

    if (!menu_mouse_scroll_enabled) {
        menu_mouse_scroll_enabled = edit_enable_menu_mouse_scroll(env);
    }
    edit_menu_mouse_enable_focus_reporting(env, eb,
                                           menu_mouse_scroll_enabled || eb->mouse_reporting_enabled,
                                           &menu_mouse_focus_reporting_added);

    const bool menu_mouse_click_enabled =
        (menu_mouse_scroll_enabled || eb->mouse_reporting_enabled);
    char mouse_suffix[EDIT_STATUS_HINT_BUFFER_LEN];
    mouse_suffix[0] = '\0';
    if (menu_mouse_click_enabled) {
        char mouse_status_text[EDIT_STATUS_HINT_BUFFER_LEN];
        edit_format_mouse_enabled_status_hint(env, false, mouse_status_text,
                                              sizeof(mouse_status_text));
        if (snprintf(mouse_suffix, sizeof(mouse_suffix), " (%s)", mouse_status_text) < 0) {
            mouse_suffix[0] = '\0';
        }
    }

    const char* footer = edit_completion_menu_footer(more_available);
    const ssize_t footer_rows = edit_menu_rendered_rows(env, eb, footer);
    char header[384];
    const char* hint_suffix = (more_available ? " (more available)" : "");
    (void)snprintf(header, sizeof(header), "[ic-info]Completions%s%s[/]\n", hint_suffix,
                   mouse_suffix);
    const ssize_t hint_help_rows = edit_menu_rendered_rows(env, eb, sbuf_string(eb->hint_help));
    const ssize_t header_rows = edit_menu_rendered_rows(env, eb, header) + hint_help_rows;
    ssize_t preview_len = -1;
    const ssize_t rendered_input_rows = edit_completion_preview_input_rows(
        env, eb, selected, header_rows + footer_rows + 2, &preview_len);
    count_displayed = count;
    if (selected >= count_displayed) {
        selected = (count_displayed > 0 ? count_displayed - 1 : -1);
        goto again;
    }

    ssize_t twidth = edit_menu_content_width(env) + 1;
    ssize_t colwidth = -1;
    ssize_t visible_count = 0;
    ssize_t max_display_width =
        edit_completions_max_width(env, count_displayed, IC_LARGE_MENU_SOURCE_LIMIT);
    colwidth = max_display_width + 6;  // extra space for prefix arrow and padding
    if (colwidth > twidth - 2) {
        colwidth = (twidth > 2 ? twidth - 2 : colwidth);
    }

    ssize_t total_rows = count_displayed;
    if (total_rows <= 0) {
        total_rows = 1;
    }

    const ssize_t rows_for_items = edit_menu_available_lines(
        env, eb, rendered_input_rows + header_rows + footer_rows + 1, 1,
        env->completion_menu_max_line_count, eb->completion_menu_maximized);
    const edit_menu_window_t window =
        edit_menu_window_for(env, total_rows, rows_for_items, selected, scroll_offset);
    const ssize_t rows_visible = window.display_count;
    const ssize_t max_scroll_offset = window.max_scroll;
    scroll_offset = window.scroll_offset;

    const ssize_t row_start = scroll_offset;
    ssize_t row_end = row_start + rows_visible - 1;
    if (row_end >= total_rows) {
        row_end = total_rows - 1;
    }

    bool wrote_any_row = false;
    for (ssize_t row = row_start; row <= row_end; row++) {
        const ssize_t idx = row;
        if (idx >= count_displayed) {
            continue;
        }
        if (wrote_any_row) {
            (void)sbuf_append(eb->extra, "\n");
        }
        wrote_any_row = true;
        editor_append_completion(env, eb, idx, colwidth, (selected == idx));
        visible_count++;
    }

    if (visible_count <= 0 && count_displayed > 0) {
        editor_append_completion(env, eb, 0, -1, (selected == 0));
        visible_count = 1;
    }

    if (sbuf_len(eb->extra) > 0) {
        (void)sbuf_append(eb->extra, "\n");
    }
    edit_menu_append_scrollbar(env, eb, &scrollbar, 0, &window);
    edit_menu_append_scroll_hint(eb->extra, count_displayed, visible_count, scroll_offset);
    (void)sbuf_append(eb->extra, footer);
    (void)sbuf_insert_at(eb->extra, header, 0);
    last_header_rows = header_rows;
    scrollbar.first_row = last_header_rows;

    last_rows_visible = rows_visible;
    last_max_scroll_offset = max_scroll_offset;

    if (!env->complete_nopreview && selected >= 0 && selected < count_displayed) {
        const char* saved_menu = sbuf_strdup(eb->extra);

        editor_start_modify(eb);
        ssize_t newpos = completions_apply(env->completions, selected, eb->input, eb->pos);
        if (newpos != IC_COMP_APPLY_FAIL) {
            if (newpos >= 0) {
                eb->pos = newpos;
            }
            if (preview_len >= 0 && preview_len < sbuf_len(eb->input)) {
                sbuf_delete_at(eb->input, preview_len, sbuf_len(eb->input) - preview_len);
                (void)sbuf_append(eb->input, "...");
                if (eb->pos > sbuf_len(eb->input)) {
                    eb->pos = sbuf_len(eb->input);
                }
            }
        }

        if (saved_menu != NULL) {
            sbuf_replace(eb->extra, saved_menu);
            mem_free(eb->mem, saved_menu);
        }

        edit_refresh(env, eb);

        editor_undo_restore(eb, false);
    } else {
        edit_refresh(env, eb);
    }

read_key:
    if (!edit_menu_read_key(env, eb, &c)) {
        c = 0;
        goto cleanup;
    }
    if (c == KEY_EVENT_RESIZE || tty_term_resize_event(env->tty)) {
        edit_menu_scrollbar_release(env, eb, &scrollbar);
        (void)edit_resize(env, eb);
        if (c == KEY_EVENT_RESIZE) {
            goto again;
        }
    }
    sbuf_clear(eb->extra);

    code_t key_no_mods = KEY_NO_MODS(c);

    if (edit_menu_scrollbar_event(env, eb, &scrollbar, c,
                                  menu_mouse_scroll_enabled || eb->mouse_reporting_enabled,
                                  &scroll_offset, &selected)) {
        c = 0;
        goto again;
    }
    if (edit_menu_mouse_prepare_key(env, eb, c, true, &menu_mouse_scroll_enabled,
                                    &menu_mouse_suspended)) {
        c = 0;
        goto again;
    }

    if (c == KEY_LINEFEED) {
        eb->completion_menu_maximized = !eb->completion_menu_maximized;
        c = 0;
        goto again;
    }

    if (key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        const bool click_selection_enabled =
            (menu_mouse_scroll_enabled || eb->mouse_reporting_enabled);
        if (click_selection_enabled) {
            bool accept_selection = false;
            if (completion_menu_mouse_select(env, eb, scroll_offset, count_displayed,
                                             last_rows_visible, last_header_rows, &selected,
                                             &accept_selection)) {
                if (accept_selection && edit_completion_click_accept_enabled(env)) {
                    c = KEY_ENTER;
                    key_no_mods = KEY_ENTER;
                } else {
                    goto again;
                }
            } else {
                if (edit_menu_mouse_event_is_left_click(env)) {
                    (void)edit_menu_mouse_suspend(env, eb, &menu_mouse_scroll_enabled,
                                                  &menu_mouse_suspended);
                }
                c = 0;
                goto again;
            }
        } else {
            c = 0;
            goto again;
        }
    }

    if (c >= '1' && c <= '9') {
        ssize_t i = (c - '1');
        const ssize_t base = scroll_offset;
        const ssize_t limit = (last_rows_visible > 0 ? last_rows_visible : count_displayed);
        ssize_t idx = base + i;
        if (i < limit && idx < count_displayed) {
            selected = idx;
            c = KEY_ENTER;
        }
    }

    bool shift_pressed = ((KEY_MODS(c) & KEY_MOD_SHIFT) != 0);
    if (shift_pressed) {
        ssize_t page = (last_rows_visible > 0 ? last_rows_visible
                                              : (count_displayed > 0 ? count_displayed : 1));
        if (page < 1) {
            page = 1;
        }
        if (key_no_mods == KEY_DOWN) {
            (void)edit_menu_page_down(env, count_displayed, page, last_max_scroll_offset,
                                      &scroll_offset, &selected);
            goto again;
        } else if (key_no_mods == KEY_UP) {
            (void)edit_menu_page_up(env, count_displayed, page, &scroll_offset, &selected);
            goto again;
        }
    }

    if (menu_mouse_scroll_enabled &&
        (key_no_mods == KEY_EVENT_MOUSE_WHEEL_UP || key_no_mods == KEY_EVENT_MOUSE_WHEEL_DOWN)) {
        if (count_displayed > 0) {
            if (key_no_mods == KEY_EVENT_MOUSE_WHEEL_DOWN) {
                if (selected < 0) {
                    selected = 0;
                } else if (selected < count_displayed - 1) {
                    selected++;
                } else {
                    term_beep(env->term);
                }
            } else {
                if (selected < 0) {
                    selected = 0;
                } else if (selected > 0) {
                    selected--;
                } else {
                    term_beep(env->term);
                }
            }
        } else {
            term_beep(env->term);
        }
        goto again;
    }

    if (c == KEY_TAB && count_displayed == 1) {
        // With a single candidate, treat tab as immediate acceptance instead of cycling
        ssize_t accept_idx = selected;
        if (accept_idx < 0 || accept_idx >= count) {
            accept_idx = (count > 0 ? 0 : -1);
        }
        if (accept_idx >= 0) {
            completion_accepted = true;
            bool applied_here = edit_complete(env, eb, accept_idx);
            if (applied_here) {
                completion_applied = true;
            }
            if (!env->completion_auto_menu) {
                edit_refresh_hint(env, eb);
            }
            if (applied_here && env->complete_autotab) {
                tty_code_pushback(env->tty, KEY_EVENT_AUTOTAB);
            }
        }
        c = 0;
        goto cleanup;
    } else if (c == KEY_DOWN || c == KEY_TAB) {
        if (count_displayed > 0) {
            if (selected < 0) {
                selected = 0;
            } else {
                selected++;
                if (selected >= count_displayed) {
                    selected = 0;
                }
            }
        }
        goto again;
    } else if (c == KEY_UP || c == KEY_SHIFT_TAB) {
        if (count_displayed > 0) {
            if (selected < 0) {
                selected = count_displayed - 1;
            } else {
                selected--;
                if (selected < 0) {
                    selected = count_displayed - 1;
                }
            }
        }
        goto again;
    } else if (c == KEY_PAGEUP) {
        c = 0;
        (void)edit_menu_page_up(env, count_displayed, last_rows_visible, &scroll_offset, &selected);
        goto again;
    } else {
        if (edit_key_is_mouse_toggle_binding(env, c)) {
            if (edit_mouse_mode_supports_editing_capture(eb->mouse_reporting_mode)) {
                edit_disable_menu_mouse_scroll(env, menu_mouse_scroll_enabled);
                menu_mouse_scroll_enabled = false;
                edit_toggle_mouse_reporting(env, eb);
                menu_mouse_scroll_enabled = edit_enable_menu_mouse_scroll(env);
                menu_mouse_suspended = false;
            }
            c = 0;
            goto again;
        }
    }

    if (c == KEY_F1) {
        edit_show_help(env, eb);
        goto again;
    } else if (c == KEY_ESC) {
        completions_clear(env->completions);
        edit_refresh(env, eb);
        c = 0;
    } else if (selected >= 0 && (c == KEY_ENTER || c == KEY_RIGHT || c == KEY_END)) {
        assert(selected < count);
        c = 0;
        completion_accepted = true;
        bool applied_here = edit_complete(env, eb, selected);
        if (applied_here) {
            completion_applied = true;
        }
        if (!env->completion_auto_menu) {
            edit_refresh_hint(env, eb);
        }
        if (applied_here && env->complete_autotab) {
            tty_code_pushback(env->tty, KEY_EVENT_AUTOTAB);
        }
    } else if (c == KEY_BACKSP) {
        edit_backspace(env, eb);
        if (!edit_recompute_completion_list(env, eb, &count, &more_available, &selected,
                                            &scroll_offset, false)) {
            sbuf_clear(eb->extra);
            edit_refresh(env, eb);
            c = 0;
            goto cleanup;
        }
        goto again;
    } else if (c == KEY_DEL) {
        edit_delete_char(env, eb);
        if (!edit_recompute_completion_list(env, eb, &count, &more_available, &selected,
                                            &scroll_offset, false)) {
            sbuf_clear(eb->extra);
            edit_refresh(env, eb);
            c = 0;
            goto cleanup;
        }
        goto again;
    } else if (!code_is_virt_key(c) || c == ' ') {
        bool inserted = false;
        char chr = 0;
        unicode_t uchr = 0;
        if (code_is_ascii_char(c, &chr)) {
            edit_insert_char(env, eb, chr);
            inserted = true;
        } else if (code_is_unicode(c, &uchr)) {
            edit_insert_unicode(env, eb, uchr);
            inserted = true;
        }
        if (inserted) {
            if (!edit_recompute_completion_list(env, eb, &count, &more_available, &selected,
                                                &scroll_offset, false)) {
                sbuf_clear(eb->extra);
                edit_refresh(env, eb);
                c = 0;
                goto cleanup;
            }
            goto again;
        }
    } else if (c == KEY_PAGEDOWN) {
        c = 0;
        if (more_available) {
            ssize_t prev_count = count;
            count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos,
                                         IC_MAX_COMPLETIONS_TO_SHOW);
            completions_sort(env->completions);
            more_available = (count >= IC_MAX_COMPLETIONS_TO_SHOW);
            if (selected >= count) {
                selected = (env->complete_nopreview ? 0 : -1);
            }
            if (count < prev_count && scroll_offset > 0 && scroll_offset >= count) {
                scroll_offset = (count > 0 ? count - 1 : 0);
            }
        } else if (last_rows_visible > 0) {
            (void)edit_menu_page_down(env, count_displayed, last_rows_visible,
                                      last_max_scroll_offset, &scroll_offset, &selected);
        }
        goto again;
    } else {
        edit_refresh(env, eb);
    }

cleanup:
    edit_menu_scrollbar_release(env, eb, &scrollbar);
    edit_menu_mouse_finish(env, eb, true, &menu_mouse_scroll_enabled, &menu_mouse_suspended,
                           &menu_mouse_focus_reporting_added);
    completions_clear(env->completions);
    if (!completion_applied && hints_enabled) {
        bool input_changed = true;
        if (saved_input != NULL) {
            const char* current_input = sbuf_string(eb->input);
            if (current_input != NULL) {
                if (strcmp(saved_input, current_input) == 0 && eb->pos == saved_pos) {
                    input_changed = false;
                }
            }
        }

        if (!input_changed) {
            sbuf_clear(eb->hint);
            if (saved_hint != NULL) {
                sbuf_replace(eb->hint, saved_hint);
            }
            sbuf_clear(eb->hint_help);
            if (saved_hint_help != NULL) {
                sbuf_replace(eb->hint_help, saved_hint_help);
            }
            edit_refresh(env, eb);
        } else {
            edit_refresh_hint(env, eb);
        }
    }

    if (saved_hint != NULL) {
        mem_free(eb->mem, saved_hint);
    }
    if (saved_hint_help != NULL) {
        mem_free(eb->mem, saved_hint_help);
    }
    if (saved_input != NULL) {
        mem_free(eb->mem, saved_input);
    }

    eb->completion_menu_active = false;
    eb->completion_menu_maximized = false;
    if (env->completion_auto_menu) {
        sbuf_clear(eb->extra);
        sbuf_clear(eb->hint);
        sbuf_clear(eb->hint_help);
        if (completion_accepted) {
            // Keep suggestions visible for the accepted text, but hand input back to the
            // editor. This also covers accepting an already-complete (no-op) candidate.
            edit_refresh_hint(env, eb);
        } else {
            edit_refresh(env, eb);
        }
    }

    if (c != 0) {
        tty_code_pushback(env->tty, c);
    }
}

static bool edit_handle_completion_auto_menu_key(ic_env_t* env, editor_t* eb, code_t key) {
    if (!eb->completion_auto_menu_visible) {
        return false;
    }

    if (key == KEY_RIGHT) {
        eb->completion_menu_maximized = false;
        if (edit_complete(env, eb, 0) && env->complete_autotab) {
            tty_code_pushback(env->tty, KEY_EVENT_AUTOTAB);
        }
        edit_refresh_hint(env, eb);
        return true;
    }

    const code_t plain = KEY_NO_MODS(key);
    const bool wheel = (eb->mouse_reporting_enabled &&
                        (plain == KEY_EVENT_MOUSE_WHEEL_UP ||
                         plain == KEY_EVENT_MOUSE_WHEEL_DOWN));
    if (key != KEY_UP && key != KEY_DOWN && !wheel) {
        return false;
    }

    // Consume the activating gesture without accepting or skipping the first candidate.
    const bool more_available = (completions_count(env->completions) >= IC_MAX_COMPLETIONS_TO_TRY);
    edit_completion_menu(env, eb, more_available, 0);
    return true;
}

static bool edit_activate_completion_auto_menu_on_click(ic_env_t* env, editor_t* eb) {
    if (!eb->completion_auto_menu_visible || !eb->mouse_reporting_enabled) {
        return false;
    }

    tty_mouse_event_t event;
    if (!tty_get_last_mouse_event(env->tty, &event) ||
        event.action != TTY_MOUSE_ACTION_LEFT_RELEASE) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &event, &target_row, &target_col, NULL)) {
        return false;
    }
    const ssize_t menu_row = target_row - eb->input_rows;
    if (menu_row < 0 || menu_row >= eb->completion_auto_menu_rows) {
        return false;
    }

    const ssize_t item = menu_row - eb->completion_auto_menu_header_rows;
    const ssize_t selected = (item >= 0 && item < eb->completion_auto_menu_item_rows ? item : 0);
    const bool more_available = (completions_count(env->completions) >= IC_MAX_COMPLETIONS_TO_TRY);
    // Activate from the displayed list, not regenerated candidates. Waiting for release consumes
    // the entire activating click, so click-to-accept cannot apply it in the newly active menu.
    // Smart-mode drag/selection handling runs before this helper in the main editor.
    edit_completion_menu(env, eb, more_available, selected);
    return true;
}

static void edit_generate_completions(ic_env_t* env, editor_t* eb, bool autotab) {
    debug_msg("edit: complete: %zd: %s\n", eb->pos, sbuf_string(eb->input));
    if (eb->pos < 0) {
        return;
    }
    ssize_t count = completions_generate(env, env->completions, sbuf_string(eb->input), eb->pos,
                                         IC_MAX_COMPLETIONS_TO_TRY);
    bool more_available = (count >= IC_MAX_COMPLETIONS_TO_TRY);
    if (env->completion_auto_menu && !autotab && count > 1 && !edit_current_line_is_empty(eb)) {
        // The first explicit completion request activates when there are multiple candidates.
        // Do not insert a common prefix or apply spell corrections before activation.
        completions_sort(env->completions);
        edit_completion_menu(env, eb, more_available, 0);
        return;
    }
    if (env->completion_auto_menu && autotab && count > 1) {
        // Auto-tab may expand unique continuations, but only explicit activation may take
        // ownership of input when the next completion has multiple choices.
        edit_refresh_hint(env, eb);
        return;
    }
    const char* first_source = (count > 0 ? completions_get_source(env->completions, 0) : NULL);
    if (first_source != NULL && strcmp(first_source, "spell") == 0) {
        bool current_word_spell = edit_completion_is_current_word_spell(env, eb, 0, NULL, NULL);
        if (!autotab && current_word_spell) {
            if (!edit_complete(env, eb, 0)) {
                term_beep(env->term);
            }
        } else if (!autotab && !current_word_spell) {
            term_beep(env->term);
        }
        completions_clear(env->completions);
        if (env->completion_auto_menu) {
            edit_refresh_hint(env, eb);
        }
        return;
    }
    if (count <= 0) {
        // no completions
        if ((!autotab) && (!edit_try_spell_correct(env, eb))) {
            term_beep(env->term);
        }

    } else if (count == 1) {
        // complete if only one match
        if (edit_complete(env, eb, 0 /*idx*/) && env->complete_autotab) {
            tty_code_pushback(env->tty, KEY_EVENT_AUTOTAB);
        }
    } else {
        // term_beep(env->term);
        if (!more_available) {
            edit_complete_longest_prefix(env, eb);
        }
        completions_sort(env->completions);
        edit_completion_menu(env, eb, more_available, 0);
    }
    if (env->completion_auto_menu) {
        edit_refresh_hint(env, eb);
    }
}
