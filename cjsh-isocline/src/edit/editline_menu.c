/*
  editline_menu.c

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

/* Shared helpers for editline menus. This file is included in editline.c. */

typedef struct edit_menu_scrollbar_s {
    ssize_t first_row;  // relative to the extra content, including wrapped headers
    ssize_t column;
    ssize_t rows;
    ssize_t thumb_row;
    ssize_t thumb_rows;
    ssize_t display_count;
    ssize_t max_scroll;
    bool pressed;
    bool dragging;
    bool discard_release;
    bool motion_reporting_added;
    ssize_t drag_top;  // absolute terminal row, retained when the pointer leaves the menu
    ssize_t drag_grab;
    ssize_t drag_last_row;
} edit_menu_scrollbar_t;

typedef struct edit_menu_session_s {
    edit_menu_scrollbar_t scrollbar;
    bool maximized;  // temporary for this invocation, never changes configured limits
    const char* prompt_text;
    const char* inline_right_text;
    bool prompt_replacement;
    bool force_prompt_visibility;
    bool prompt_begins_with_newline;
    ssize_t prompt_prefix_lines;
    ssize_t line_number_column_width;
    ssize_t inline_right_width;
    bool old_hint;
    bool old_highlight;
    bool mouse_scroll_enabled;
    bool mouse_suspended;
    bool mouse_focus_reporting_added;
    bool prompt_prefix_hidden;
} edit_menu_session_t;

typedef struct edit_menu_window_s {
    ssize_t display_count;
    ssize_t max_scroll;
    ssize_t scroll_offset;
} edit_menu_window_t;

static ssize_t edit_menu_content_width(ic_env_t* env) {
    const ssize_t margin = (env->line_wrap_marker_width > 1 ? env->line_wrap_marker_width : 1);
    const ssize_t width = term_get_width(env->term) - margin - 1;
    return (width > 0 ? width : 1);
}

static void edit_menu_scrollbar_release(ic_env_t* env, editor_t* eb, edit_menu_scrollbar_t* bar) {
    if (bar->motion_reporting_added) {
        term_write(env->term, "\x1b[?1002l");
        if (eb->mouse_capture_depth > 0) {
            term_write(env->term, "\x1b[?1000h");
        }
        term_flush(env->term);
        bar->motion_reporting_added = false;
    }
    if (bar->pressed) {
        eb->mouse_left_button_down = false;
        bar->discard_release = true;
    }
    bar->pressed = false;
    bar->dragging = false;
}

static void edit_menu_mouse_enable_focus_reporting(ic_env_t* env, editor_t* eb, bool mouse_active,
                                                   bool* added) {
    if (!mouse_active || eb == NULL || added == NULL || *added ||
        eb->mouse_focus_reporting_enabled) {
        return;
    }

    edit_set_mouse_focus_reporting(env, eb, true);
    *added = eb->mouse_focus_reporting_enabled;
}

static bool edit_menu_mouse_suspend(ic_env_t* env, editor_t* eb, bool* scroll_enabled,
                                    bool* suspended) {
    if (eb == NULL || scroll_enabled == NULL || suspended == NULL || *suspended ||
        (!*scroll_enabled && !eb->mouse_reporting_enabled)) {
        return false;
    }

    *scroll_enabled = false;
    if (eb->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART) {
        edit_set_mouse_auto_suspended(env, eb, true, true);
    } else {
        edit_set_mouse_reporting_enabled(env, eb, false);
    }
    *suspended = true;
    return true;
}

static void edit_menu_mouse_resume(ic_env_t* env, editor_t* eb, bool want_scroll,
                                   bool* scroll_enabled, bool* suspended) {
    if (eb == NULL || scroll_enabled == NULL || suspended == NULL || !*suspended) {
        return;
    }

    if (eb->mouse_reporting_mode == IC_MOUSE_CLICKING_SMART) {
        edit_set_mouse_auto_suspended(env, eb, false, false);
    } else {
        edit_apply_mouse_reporting_policy(env, eb);
    }
    *scroll_enabled = (want_scroll ? edit_enable_menu_mouse_scroll(env) : false);
    *suspended = false;
}

static bool edit_menu_mouse_prepare_key(ic_env_t* env, editor_t* eb, code_t key, bool want_scroll,
                                        bool* scroll_enabled, bool* suspended) {
    if (eb == NULL || scroll_enabled == NULL || suspended == NULL) {
        return false;
    }

    if (edit_maybe_resume_smart_mouse_reporting(env, eb, key)) {
        if (*suspended && !eb->mouse_reporting_auto_suspended) {
            *scroll_enabled = (want_scroll ? edit_enable_menu_mouse_scroll(env) : false);
            *suspended = false;
        }
        return true;
    }

    const code_t key_no_mods = KEY_NO_MODS(key);
    if (key_no_mods != KEY_EVENT_MOUSE_OTHER && key_no_mods != KEY_NONE) {
        eb->mouse_left_button_down = false;
    }
    if (key_no_mods == KEY_EVENT_FOCUS_OUT) {
        (void)edit_menu_mouse_suspend(env, eb, scroll_enabled, suspended);
        return true;
    }
    if (key_no_mods == KEY_EVENT_FOCUS_IN) {
        edit_menu_mouse_resume(env, eb, want_scroll, scroll_enabled, suspended);
        return true;
    }
    if (*suspended && key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        return true;
    }
    if (key_no_mods == KEY_EVENT_MOUSE_OTHER && (*scroll_enabled || eb->mouse_reporting_enabled)) {
        tty_mouse_event_t event;
        if (tty_get_last_mouse_event(env->tty, &event) && edit_mouse_event_is_drag(eb, &event)) {
            return edit_menu_mouse_suspend(env, eb, scroll_enabled, suspended);
        }
    }
    if (*suspended && !edit_key_is_mouse_toggle_binding(env, key) &&
        edit_mouse_auto_resume_triggered_by_key(key)) {
        edit_menu_mouse_resume(env, eb, want_scroll, scroll_enabled, suspended);
    }
    return false;
}

static void edit_menu_mouse_finish(ic_env_t* env, editor_t* eb, bool want_scroll,
                                   bool* scroll_enabled, bool* suspended,
                                   bool* focus_reporting_added) {
    if (scroll_enabled == NULL || suspended == NULL || focus_reporting_added == NULL) {
        return;
    }

    edit_menu_mouse_resume(env, eb, want_scroll, scroll_enabled, suspended);
    edit_disable_menu_mouse_scroll(env, *scroll_enabled);
    *scroll_enabled = false;
    if (*focus_reporting_added) {
        edit_set_mouse_focus_reporting(env, eb, false);
        *focus_reporting_added = false;
    }
}

static const char* edit_menu_tag_style(bool selected) {
    return selected ? "ic-menu-selected-secondary" : "ic-diminish";
}

static void edit_menu_append_tag_text(stringbuf_t* sb, bool selected, const char* text) {
    if (sb == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    (void)sbuf_appendf(sb, "[%s]", edit_menu_tag_style(selected));
    (void)sbuf_append(sb, text);
    (void)sbuf_append(sb, "[/]");
}

static void edit_menu_append_tag_text_n(stringbuf_t* sb, bool selected, const char* text,
                                        ssize_t len) {
    if (sb == NULL || text == NULL || len <= 0) {
        return;
    }
    (void)sbuf_appendf(sb, "[%s]", edit_menu_tag_style(selected));
    (void)sbuf_append_n(sb, text, len);
    (void)sbuf_append(sb, "[/]");
}

static bool edit_menu_should_syntax_highlight_item_ex(const ic_env_t* env, bool selected,
                                                      bool syntax_highlighting_enabled) {
    if (env == NULL || !syntax_highlighting_enabled || env->highlighter == NULL) {
        return false;
    }
    switch (env->menu_highlight_mode) {
        case IC_MENU_HIGHLIGHT_ALL:
            return true;
        case IC_MENU_HIGHLIGHT_SINGLE:
            return selected;
        case IC_MENU_HIGHLIGHT_REVERSE:
            return !selected;
        case IC_MENU_HIGHLIGHT_NONE:
        default:
            return false;
    }
}

static bool edit_menu_should_syntax_highlight_item(const ic_env_t* env, bool selected) {
    return edit_menu_should_syntax_highlight_item_ex(env, selected,
                                                     env != NULL && !env->no_highlight);
}

static bool edit_menu_color_is_rgb(ic_color_t color) {
    return color >= IC_RGB(0);
}

static int edit_menu_ansi_color_index(ic_color_t color) {
    if (color >= IC_ANSI_BLACK && color <= IC_ANSI_SILVER) {
        return (int)(color - IC_ANSI_BLACK);
    }
    if (color >= IC_ANSI_GRAY && color <= IC_ANSI_WHITE) {
        return 8 + (int)(color - IC_ANSI_GRAY);
    }
    if (color == IC_ANSI_DEFAULT) {
        return 256;
    }
    return -1;
}

static void edit_menu_append_tag_separator(stringbuf_t* sb, bool* first) {
    if (!*first) {
        (void)sbuf_append_char(sb, ' ');
    }
    *first = false;
}

static bool edit_menu_append_color_property(stringbuf_t* sb, bool* first, const char* rgb_name,
                                            const char* ansi_name, ic_color_t color) {
    if (color == IC_COLOR_NONE) {
        return false;
    }

    int ansi_index = edit_menu_ansi_color_index(color);
    bool is_rgb = edit_menu_color_is_rgb(color);
    if (ansi_index < 0 && !is_rgb) {
        return false;
    }

    edit_menu_append_tag_separator(sb, first);
    if (ansi_index >= 0) {
        (void)sbuf_appendf(sb, "%s=%d", ansi_name, ansi_index);
    } else {
        (void)sbuf_appendf(sb, "%s=#%06x", rgb_name, (color & 0xFFFFFFu));
    }
    return true;
}

static bool edit_menu_append_attr_open(stringbuf_t* sb, attr_t attr) {
    if (sb == NULL || attr_is_none(attr)) {
        return false;
    }

    bool first = true;
    ssize_t tag_start = sbuf_len(sb);
    (void)sbuf_append_char(sb, '[');
    if (attr.x.bold == IC_ON) {
        edit_menu_append_tag_separator(sb, &first);
        (void)sbuf_append(sb, "b");
    }
    if (attr.x.italic == IC_ON) {
        edit_menu_append_tag_separator(sb, &first);
        (void)sbuf_append(sb, "i");
    }
    if (attr.x.underline == IC_ON) {
        edit_menu_append_tag_separator(sb, &first);
        (void)sbuf_append(sb, "u");
    }
    if (attr.x.reverse == IC_ON) {
        edit_menu_append_tag_separator(sb, &first);
        (void)sbuf_append(sb, "r");
    }
    (void)edit_menu_append_color_property(sb, &first, "color", "ansi-color", attr.x.color);
    (void)edit_menu_append_color_property(sb, &first, "bgcolor", "ansi-bgcolor", attr.x.bgcolor);
    (void)edit_menu_append_color_property(sb, &first, "underline-color", "ansi-underline-color",
                                          attr.x.underline_color);

    if (first) {
        sbuf_delete_from(sb, tag_start);
        return false;
    }
    (void)sbuf_append_char(sb, ']');
    return true;
}

static void edit_menu_append_escaped_n(stringbuf_t* sb, const char* text, ssize_t len) {
    if (sb == NULL || text == NULL || len <= 0) {
        return;
    }

    ssize_t segment_start = 0;
    for (ssize_t i = 0; i < len; ++i) {
        if (text[i] != '[' && text[i] != '\\') {
            continue;
        }
        if (i > segment_start) {
            (void)sbuf_append_n(sb, text + segment_start, i - segment_start);
        }
        (void)sbuf_append_char(sb, '\\');
        (void)sbuf_append_char(sb, text[i]);
        segment_start = i + 1;
    }
    if (segment_start < len) {
        (void)sbuf_append_n(sb, text + segment_start, len - segment_start);
    }
}

static bool edit_menu_is_line_break_at(const char* text, ssize_t len, ssize_t pos,
                                       ssize_t* advance);

static void edit_menu_append_attr_slice(stringbuf_t* sb, const char* text, ssize_t len,
                                        const attr_t* attr_data, ssize_t attr_offset,
                                        ssize_t attr_len, ssize_t match_pos, ssize_t match_len,
                                        bool selected, bool highlight_match,
                                        const char* newline_indent) {
    if (sb == NULL || text == NULL || len <= 0) {
        return;
    }
    if (attr_data == NULL || attr_offset < 0 || attr_len < 0 || attr_offset + len > attr_len) {
        edit_menu_append_escaped_n(sb, text, len);
        return;
    }

    ssize_t underline_start = -1;
    ssize_t underline_end = -1;
    if (highlight_match && match_len > 0 && match_pos >= 0 && match_pos < len) {
        underline_start = match_pos;
        underline_end = match_pos + match_len;
        if (underline_end > len) {
            underline_end = len;
        }
        if (underline_end <= underline_start) {
            underline_start = -1;
            underline_end = -1;
        }
    }

    bool underline_open = false;
    ssize_t pos = 0;
    while (pos < len) {
        ssize_t line_break_advance = 0;
        if (edit_menu_is_line_break_at(text, len, pos, &line_break_advance)) {
            if (underline_open) {
                (void)sbuf_append(sb, "[/u]");
                underline_open = false;
            }
            (void)sbuf_append_char(sb, '\n');
            if (newline_indent != NULL) {
                (void)sbuf_append(sb, newline_indent);
            }
            pos += line_break_advance;
            continue;
        }

        bool should_underline =
            (underline_start >= 0 && pos >= underline_start && pos < underline_end);
        if (should_underline != underline_open) {
            if (underline_open) {
                (void)sbuf_append(sb, "[/u]");
            } else if (selected) {
                (void)sbuf_append(sb, "[u]");
            } else {
                (void)sbuf_append(sb, "[u ic-emphasis]");
            }
            underline_open = should_underline;
        }

        attr_t attr = attr_data[attr_offset + pos];
        ssize_t run_end = pos + 1;
        while (run_end < len) {
            if (edit_menu_is_line_break_at(text, len, run_end, NULL)) {
                break;
            }
            bool run_underline =
                (underline_start >= 0 && run_end >= underline_start && run_end < underline_end);
            if (run_underline != should_underline ||
                !attr_is_eq(attr, attr_data[attr_offset + run_end])) {
                break;
            }
            run_end++;
        }

        bool attr_open = edit_menu_append_attr_open(sb, attr);
        edit_menu_append_escaped_n(sb, text + pos, run_end - pos);
        if (attr_open) {
            (void)sbuf_append(sb, "[/]");
        }
        pos = run_end;
    }

    if (underline_open) {
        (void)sbuf_append(sb, "[/u]");
    }
}

static char* edit_menu_plain_text(alloc_t* mem, bbcode_t* bbcode, const char* text, ssize_t len,
                                  bool parse_bbcode) {
    if (mem == NULL || text == NULL) {
        return NULL;
    }
    if (len < 0) {
        len = ic_strlen(text);
    }
    if (!parse_bbcode) {
        return mem_strndup(mem, text, len);
    }

    char* text_copy = mem_strndup(mem, text, len);
    if (text_copy == NULL) {
        return NULL;
    }
    stringbuf_t* plain = sbuf_new(mem);
    attrbuf_t* ignored_attrs = attrbuf_new(mem);
    char* result = NULL;
    if (plain != NULL && ignored_attrs != NULL) {
        bbcode_append(bbcode, text_copy, plain, ignored_attrs);
        result = mem_strdup(mem, sbuf_string(plain));
    }
    attrbuf_free(ignored_attrs);
    sbuf_free(plain);
    mem_free(mem, text_copy);
    return result;
}

static bool edit_menu_is_line_break_at(const char* text, ssize_t len, ssize_t pos,
                                       ssize_t* advance) {
    if (advance != NULL) {
        *advance = 0;
    }
    if (text == NULL || pos < 0 || pos >= len) {
        return false;
    }
    if (text[pos] == '\r') {
        if (advance != NULL) {
            *advance = (pos + 1 < len && text[pos + 1] == '\n') ? 2 : 1;
        }
        return true;
    }
    if (text[pos] == '\n') {
        if (advance != NULL) {
            *advance = 1;
        }
        return true;
    }
    return false;
}

static void edit_menu_append_syntax_highlighted_plain(ic_env_t* env, stringbuf_t* sb,
                                                      const char* text, ssize_t len,
                                                      ssize_t match_pos, ssize_t match_len,
                                                      bool selected, bool highlight_match,
                                                      const char* newline_indent) {
    if (env == NULL || sb == NULL || text == NULL || len <= 0) {
        return;
    }

    attrbuf_t* attrs = attrbuf_new(env->mem);
    if (attrs == NULL) {
        edit_menu_append_escaped_n(sb, text, len);
        return;
    }
    highlight(env->mem, env->bbcode, text, attrs, env->highlighter, env->highlighter_arg);
    const attr_t* attr_data = attrbuf_attrs(attrs, len);
    if (attr_data == NULL) {
        attrbuf_free(attrs);
        edit_menu_append_escaped_n(sb, text, len);
        return;
    }

    edit_menu_append_attr_slice(sb, text, len, attr_data, 0, len, match_pos, match_len, selected,
                                highlight_match, newline_indent);
    attrbuf_free(attrs);
}

static void edit_menu_append_syntax_highlighted_text(ic_env_t* env, stringbuf_t* sb,
                                                     const char* text, ssize_t len,
                                                     bool parse_bbcode, ssize_t match_pos,
                                                     ssize_t match_len, bool selected,
                                                     bool highlight_match,
                                                     const char* newline_indent) {
    if (env == NULL || sb == NULL || text == NULL) {
        return;
    }
    char* plain = edit_menu_plain_text(env->mem, env->bbcode, text, len, parse_bbcode);
    if (plain == NULL) {
        return;
    }
    edit_menu_append_syntax_highlighted_plain(env, sb, plain, ic_strlen(plain), match_pos,
                                              match_len, selected, highlight_match, newline_indent);
    mem_free(env->mem, plain);
}

static bool edit_menu_append_completion_syntax_highlighted_text(
    ic_env_t* env, editor_t* eb, stringbuf_t* sb, completions_t* completions, ssize_t idx,
    const char* text, ssize_t len, bool parse_bbcode, ssize_t match_pos, ssize_t match_len,
    bool selected, bool highlight_match, const char* newline_indent) {
    if (env == NULL || eb == NULL || sb == NULL || completions == NULL || text == NULL ||
        env->highlighter == NULL || eb->input == NULL) {
        return false;
    }

    const char* input = sbuf_string(eb->input);
    const char* replacement = NULL;
    ssize_t replacement_start = 0;
    ssize_t delete_after = 0;
    if (!completions_get_apply_range(completions, idx, input, eb->pos, &replacement,
                                     &replacement_start, &delete_after)) {
        return false;
    }
    if (replacement == NULL) {
        return false;
    }

    if (len < 0) {
        len = ic_strlen(text);
    }
    char* plain = edit_menu_plain_text(env->mem, env->bbcode, text, len, parse_bbcode);
    if (plain == NULL) {
        return false;
    }

    bool appended = false;
    const ssize_t plain_len = ic_strlen(plain);
    const ssize_t replacement_len = ic_strlen(replacement);
    if (plain_len <= 0) {
        appended = true;
        goto done_plain;
    }
    if (plain_len > replacement_len || memcmp(plain, replacement, (size_t)plain_len) != 0) {
        goto done_plain;
    }

    stringbuf_t* accepted = sbuf_new(env->mem);
    attrbuf_t* attrs = attrbuf_new(env->mem);
    if (accepted == NULL || attrs == NULL) {
        sbuf_free(accepted);
        attrbuf_free(attrs);
        goto done_plain;
    }

    const ssize_t input_len = ic_strlen(input);
    ssize_t suffix_start = eb->pos + delete_after;
    if (suffix_start < 0) {
        suffix_start = 0;
    }
    if (suffix_start > input_len) {
        suffix_start = input_len;
    }
    if (replacement_start > input_len) {
        replacement_start = input_len;
    }

    (void)sbuf_append_n(accepted, input, replacement_start);
    const ssize_t attr_start = sbuf_len(accepted);
    (void)sbuf_append(accepted, replacement);
    if (suffix_start < input_len) {
        (void)sbuf_append_n(accepted, input + suffix_start, input_len - suffix_start);
    }

    const ssize_t accepted_len = sbuf_len(accepted);
    const char* accepted_text = sbuf_string(accepted);
    highlight(env->mem, env->bbcode, accepted_text, attrs, env->highlighter, env->highlighter_arg);
    const attr_t* attr_data = attrbuf_attrs(attrs, accepted_len);
    if (attr_data != NULL) {
        edit_menu_append_attr_slice(sb, plain, plain_len, attr_data, attr_start, accepted_len,
                                    match_pos, match_len, selected, highlight_match,
                                    newline_indent);
        appended = true;
    }

    attrbuf_free(attrs);
    sbuf_free(accepted);

done_plain:
    mem_free(env->mem, plain);
    return appended;
}

static edit_menu_session_t edit_menu_begin(ic_env_t* env, editor_t* eb, const char* prompt_text,
                                           bool enable_mouse_scroll) {
    edit_menu_session_t session = {0};
    if (eb == NULL) {
        return session;
    }

    editor_undo_capture(eb);
    eb->disable_undo = true;
    session.old_hint = ic_enable_hint(false);
    session.old_highlight = ic_enable_highlight(true);
    (void)ic_enable_highlight(session.old_highlight);
    session.prompt_text = eb->prompt_text;
    session.inline_right_text = eb->inline_right_text;
    session.prompt_replacement = eb->replace_prompt_line_with_number;
    session.force_prompt_visibility = eb->force_prompt_text_visible;
    session.prompt_begins_with_newline = eb->prompt_begins_with_newline;
    session.prompt_prefix_lines = eb->prompt_prefix_lines;
    session.line_number_column_width = eb->line_number_column_width;
    session.inline_right_width = eb->inline_right_width;
    session.prompt_prefix_hidden = (eb->prompt_prefix_lines > 0);
    if (session.prompt_prefix_hidden) {
        edit_clear_with_prompt_prefix(env, eb, eb->prompt_prefix_lines);
        eb->prompt_prefix_lines = 0;
        eb->prompt_begins_with_newline = false;
        eb->cur_rows = 1;
        eb->input_rows = 1;
        eb->cur_row = 0;
        eb->view_first_row = 0;
        eb->view_rows = 1;
        eb->view_input_rows = 1;
    }
    eb->force_prompt_text_visible = true;
    eb->replace_prompt_line_with_number = false;
    eb->prompt_text = prompt_text;
    eb->inline_right_text = NULL;
    eb->inline_right_width = 0;
    session.mouse_scroll_enabled =
        (enable_mouse_scroll ? edit_enable_menu_mouse_scroll(env) : false);
    edit_menu_mouse_enable_focus_reporting(env, eb, session.mouse_scroll_enabled,
                                           &session.mouse_focus_reporting_added);
    return session;
}

static void edit_menu_finish(ic_env_t* env, editor_t* eb, edit_menu_session_t* session,
                             bool restore_undo, bool refresh) {
    if (eb == NULL || session == NULL) {
        return;
    }

    edit_menu_scrollbar_release(env, eb, &session->scrollbar);
    sbuf_clear(eb->extra);
    eb->disable_undo = false;
    if (restore_undo) {
        editor_undo_restore(eb, false);
    }

    if (session->prompt_prefix_hidden) {
        edit_clear_with_prompt_prefix(env, eb, 0);
        eb->cur_rows = 1;
        eb->input_rows = 1;
        eb->cur_row = 0;
        eb->view_first_row = 0;
        eb->view_rows = 1;
        eb->view_input_rows = 1;
    }

    eb->prompt_text = session->prompt_text;
    eb->inline_right_text = session->inline_right_text;
    eb->replace_prompt_line_with_number = session->prompt_replacement;
    eb->force_prompt_text_visible = session->force_prompt_visibility;
    eb->prompt_begins_with_newline = session->prompt_begins_with_newline;
    eb->prompt_prefix_lines = session->prompt_prefix_lines;
    eb->line_number_column_width = session->line_number_column_width;
    eb->inline_right_width = session->inline_right_width;
    (void)ic_enable_hint(session->old_hint);
    (void)ic_enable_highlight(session->old_highlight);
    edit_menu_mouse_finish(env, eb, true, &session->mouse_scroll_enabled, &session->mouse_suspended,
                           &session->mouse_focus_reporting_added);
    if (session->prompt_prefix_hidden) {
        redraw_prompt_prefix_lines(env, eb);
    }
    if (refresh || session->prompt_prefix_hidden) {
        edit_refresh(env, eb);
    }
}

static ssize_t edit_menu_input_rows(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL) {
        return 1;
    }

    ssize_t promptw = 0;
    ssize_t cpromptw = 0;
    edit_get_prompt_width(env, eb, false, &promptw, &cpromptw);

    ssize_t input_len = sbuf_len(eb->input);
    if (input_len < 0) {
        input_len = 0;
    }

    rowcol_t rc_dummy;
    memset(&rc_dummy, 0, sizeof(rc_dummy));
    ssize_t input_rows = sbuf_get_rc_at_pos(eb->input, eb->termw, promptw, cpromptw,
                                            env->line_wrap_marker_width, input_len, &rc_dummy);
    if (input_rows <= 0) {
        input_rows = 1;
    }
    return edit_visible_input_row_count(env, eb, input_rows);
}

static ssize_t edit_menu_available_lines(ic_env_t* env, editor_t* eb, ssize_t reserved_rows,
                                         ssize_t min_lines, size_t max_lines, bool maximized) {
    if (env == NULL || eb == NULL) {
        return min_lines;
    }

    ssize_t available_lines = edit_available_terminal_rows(env, eb) - reserved_rows;
    if (available_lines < min_lines) {
        available_lines = min_lines;
    }
    if (!maximized && available_lines > (ssize_t)max_lines) {
        available_lines = (ssize_t)max_lines;
    }
    return available_lines;
}

static ssize_t edit_menu_rendered_rows(ic_env_t* env, editor_t* eb, const char* text) {
    if (env == NULL || eb == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }

    stringbuf_t* rendered = sbuf_new(env->mem);
    if (rendered == NULL) {
        return 1;
    }

    bbcode_append(env->bbcode, text, rendered, NULL);
    rowcol_t rc_dummy;
    memset(&rc_dummy, 0, sizeof(rc_dummy));
    ssize_t rows = sbuf_get_rc_at_pos(rendered, term_get_width(env->term), 0, 0,
                                      env->line_wrap_marker_width, sbuf_len(rendered), &rc_dummy);
    if (sbuf_ends_with_newline(rendered) && rows > 0) {
        rows--;
    }
    sbuf_free(rendered);
    return (rows > 0 ? rows : 1);
}

static edit_menu_window_t edit_menu_window_for(ic_env_t* env, ssize_t item_count,
                                               ssize_t requested_rows, ssize_t selected_idx,
                                               ssize_t scroll_offset) {
    if (requested_rows < 1) {
        requested_rows = 1;
    }

    const editline_viewport_t viewport =
        editline_viewport_for(item_count, 0, selected_idx, requested_rows, (size_t)requested_rows,
                              env->multiline_bottom_line_count, scroll_offset);
    edit_menu_window_t window = {
        .display_count = viewport.input_row_count,
        .max_scroll =
            (item_count > viewport.input_row_count ? item_count - viewport.input_row_count : 0),
        .scroll_offset = viewport.input_first_row,
    };
    return window;
}

static ssize_t edit_menu_page_selection(ic_env_t* env, ssize_t page, ssize_t scroll_offset) {
    const ssize_t max_margin = (page - 1) / 2;
    const ssize_t margin = (env->multiline_bottom_line_count < (size_t)max_margin
                                ? (ssize_t)env->multiline_bottom_line_count
                                : max_margin);
    // Keep the selection inside the new page's margins so the next render preserves the page.
    return scroll_offset + margin;
}

// Scrollbar gestures own their entire press/release sequence. Handle them before smart
// mouse selection can relinquish capture or an item click can accept a menu entry.
static bool edit_menu_scrollbar_event(ic_env_t* env, editor_t* eb, edit_menu_scrollbar_t* bar,
                                      code_t key, bool mouse_enabled, ssize_t* scroll_offset,
                                      ssize_t* selected) {
    if (KEY_NO_MODS(key) != KEY_EVENT_MOUSE_OTHER) {
        if (key != KEY_NONE) {
            edit_menu_scrollbar_release(env, eb, bar);
        }
        return false;
    }
    if (!mouse_enabled) {
        edit_menu_scrollbar_release(env, eb, bar);
        return false;
    }

    tty_mouse_event_t event;
    if (!tty_get_last_mouse_event(env->tty, &event)) {
        return false;
    }
    if (event.action == TTY_MOUSE_ACTION_LEFT_PRESS) {
        bar->discard_release = false;
    } else if (bar->discard_release && (event.action == TTY_MOUSE_ACTION_LEFT_DRAG ||
                                        event.action == TTY_MOUSE_ACTION_LEFT_RELEASE)) {
        if (event.action == TTY_MOUSE_ACTION_LEFT_RELEASE) {
            bar->discard_release = false;
        }
        return true;
    }
    if (bar->pressed && event.action != TTY_MOUSE_ACTION_LEFT_PRESS) {
        if (bar->dragging && event.row != bar->drag_last_row && bar->rows > bar->thumb_rows &&
            (event.action == TTY_MOUSE_ACTION_LEFT_DRAG ||
             event.action == TTY_MOUSE_ACTION_LEFT_RELEASE)) {
            const ssize_t travel = bar->rows - bar->thumb_rows;
            ssize_t position = event.row - bar->drag_top - bar->drag_grab;
            if (position < 0) {
                position = 0;
            }
            if (position > travel) {
                position = travel;
            }
            const ssize_t offset =
                (ssize_t)(((double)position * (double)bar->max_scroll / (double)travel) + 0.5);
            bar->drag_last_row = event.row;
            if (offset != *scroll_offset) {
                *scroll_offset = offset;
                *selected = edit_menu_page_selection(env, bar->display_count, offset);
            }
        }
        if (event.action == TTY_MOUSE_ACTION_LEFT_RELEASE) {
            edit_menu_scrollbar_release(env, eb, bar);
            bar->discard_release = false;
        }
        return true;
    }
    if (bar->rows <= 0 || event.action != TTY_MOUSE_ACTION_LEFT_PRESS) {
        return false;
    }

    ssize_t row = 0;
    ssize_t column = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &event, &row, &column, NULL)) {
        return false;
    }
    row -= eb->input_rows + bar->first_row;
    if (column != bar->column || row < 0 || row >= bar->rows) {
        return false;
    }

    edit_menu_scrollbar_release(env, eb, bar);
    bar->discard_release = false;
    bar->pressed = true;
    eb->mouse_left_button_down = false;
    if (bar->rows > 1 && row >= bar->thumb_row && row < bar->thumb_row + bar->thumb_rows) {
        bar->dragging = true;
        bar->drag_top = event.row - row;
        bar->drag_grab = row - bar->thumb_row;
        bar->drag_last_row = event.row;
        if (eb->mouse_reporting_mode != IC_MOUSE_CLICKING_SMART) {
            term_write(env->term, "\x1b[?1002h");
            term_flush(env->term);
            bar->motion_reporting_added = true;
        }
    } else {
        // A one-row viewport has no track; clicking it advances by one page.
        const ssize_t delta = (row < bar->thumb_row ? -bar->display_count : bar->display_count);
        *scroll_offset += delta;
        if (*scroll_offset < 0) {
            *scroll_offset = 0;
        }
        if (*scroll_offset > bar->max_scroll) {
            *scroll_offset = bar->max_scroll;
        }
        *selected = edit_menu_page_selection(env, bar->display_count, *scroll_offset);
    }
    return true;
}

typedef struct edit_menu_scrollbar_render_s {
    ic_env_t* env;
    stringbuf_t* output;
    const attr_t* attrs;
    ssize_t length;
    edit_menu_scrollbar_t* bar;
} edit_menu_scrollbar_render_t;

static bool edit_menu_scrollbar_render_row(const char* text, ssize_t row, ssize_t start,
                                           ssize_t length, ssize_t start_width, bool wrapped,
                                           const void* arg, void* result) {
    ic_unused(start_width);
    ic_unused(wrapped);
    ic_unused(result);
    const edit_menu_scrollbar_render_t* render = (const edit_menu_scrollbar_render_t*)arg;
    const edit_menu_scrollbar_t* bar = render->bar;
    if (row >= bar->rows) {
        return true;
    }
    edit_menu_append_attr_slice(render->output, text + start, length, render->attrs, start,
                                render->length, -1, 0, false, false, NULL);
    const ssize_t width = str_column_width_n(text + start, length);
    for (ssize_t col = width; col < bar->column; ++col) {
        (void)sbuf_append_char(render->output, ' ');
    }
    const bool thumb = (row >= bar->thumb_row && row < bar->thumb_row + bar->thumb_rows);
    const bool utf8 = tty_is_utf8(render->env->tty);
    (void)sbuf_appendf(render->output, "[%s]%s[/]\n", thumb ? "ic-emphasis" : "ic-diminish",
                       thumb ? (utf8 ? "\xE2\x96\x88" : "#") : (utf8 ? "\xE2\x94\x82" : "|"));
    return false;
}

// Decorate only item rows, preserving their attributes and leaving headers/help full width.
// The last terminal cell stays empty to avoid delayed-wrap behavior on terminal emulators.
static void edit_menu_append_scrollbar(ic_env_t* env, editor_t* eb, edit_menu_scrollbar_t* bar,
                                       ssize_t items_start, const edit_menu_window_t* window) {
    bar->rows = 0;
    const ssize_t content_width = edit_menu_content_width(env);
    if (window->max_scroll <= 0 || window->display_count <= 0 || content_width < 4) {
        return;
    }
    stringbuf_t* plain = sbuf_new(env->mem);
    attrbuf_t* attrs = attrbuf_new(env->mem);
    if (plain == NULL || attrs == NULL) {
        sbuf_free(plain);
        attrbuf_free(attrs);
        return;
    }
    bbcode_append(env->bbcode, sbuf_string(eb->extra) + items_start, plain, attrs);
    const ssize_t length = sbuf_len(plain);
    const bool trailing_newline = sbuf_ends_with_newline(plain);
    rowcol_t rc = {0};
    // Also respect the renderer's margin for a custom, wider wrap marker.
    bar->column = content_width;
    bar->rows = sbuf_get_rc_at_pos(plain, content_width + 1, 0, 0, 1, length, &rc) -
                (trailing_newline ? 1 : 0);
    bar->display_count = window->display_count;
    bar->max_scroll = window->max_scroll;
    bar->thumb_rows = (ssize_t)((double)bar->rows * (double)window->display_count /
                                (double)(window->max_scroll + window->display_count));
    if (bar->thumb_rows < 1) {
        bar->thumb_rows = 1;
    }
    bar->thumb_row = (ssize_t)((double)(bar->rows - bar->thumb_rows) *
                                   (double)window->scroll_offset / (double)window->max_scroll +
                               0.5);

    sbuf_delete_at(eb->extra, items_start, sbuf_len(eb->extra) - items_start);
    bar->first_row = edit_menu_rendered_rows(env, eb, sbuf_string(eb->extra)) +
                     edit_menu_rendered_rows(env, eb, sbuf_string(eb->hint_help));
    edit_menu_scrollbar_render_t render = {
        env, eb->extra, attrbuf_attrs(attrs, length), length, bar,
    };
    (void)sbuf_for_each_row(plain, content_width + 1, 0, 0, 1, &edit_menu_scrollbar_render_row,
                            &render, NULL);
    if (!trailing_newline && sbuf_ends_with_newline(eb->extra)) {
        sbuf_delete_at(eb->extra, sbuf_len(eb->extra) - 1, 1);
    }
    attrbuf_free(attrs);
    sbuf_free(plain);
}

static bool edit_menu_page_down(ic_env_t* env, ssize_t item_count, ssize_t page, ssize_t max_scroll,
                                ssize_t* scroll_offset, ssize_t* selected_idx) {
    if (scroll_offset == NULL || selected_idx == NULL || item_count <= 0 || page <= 0) {
        term_beep(env->term);
        return false;
    }
    if (*scroll_offset >= max_scroll) {
        term_beep(env->term);
        return false;
    }

    *scroll_offset += page;
    if (*scroll_offset > max_scroll) {
        *scroll_offset = max_scroll;
    }
    *selected_idx = edit_menu_page_selection(env, page, *scroll_offset);
    if (*selected_idx >= item_count) {
        *selected_idx = item_count - 1;
    }
    if (*selected_idx < 0) {
        *selected_idx = 0;
    }
    return true;
}

static bool edit_menu_page_up(ic_env_t* env, ssize_t item_count, ssize_t page,
                              ssize_t* scroll_offset, ssize_t* selected_idx) {
    if (scroll_offset == NULL || selected_idx == NULL || item_count <= 0 || page <= 0) {
        term_beep(env->term);
        return false;
    }
    if (*scroll_offset <= 0) {
        term_beep(env->term);
        return false;
    }

    if (*scroll_offset > page) {
        *scroll_offset -= page;
    } else {
        *scroll_offset = 0;
    }
    *selected_idx = edit_menu_page_selection(env, page, *scroll_offset);
    if (*selected_idx >= item_count) {
        *selected_idx = item_count - 1;
    }
    if (*selected_idx < 0) {
        *selected_idx = 0;
    }
    return true;
}

static bool edit_menu_move_selection(ic_env_t* env, ssize_t item_count, ssize_t delta,
                                     ssize_t* selected_idx) {
    if (selected_idx == NULL || item_count <= 0) {
        term_beep(env->term);
        return false;
    }

    ssize_t next = *selected_idx + delta;
    if (next < 0 || next >= item_count) {
        term_beep(env->term);
        return false;
    }
    *selected_idx = next;
    return true;
}

static const char* edit_menu_first_line_end(const char* str) {
    if (str == NULL) {
        return NULL;
    }
    const char* p = str;
    while (*p && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

static bool edit_menu_contains_line_break(const char* str) {
    if (str == NULL) {
        return false;
    }
    return (strchr(str, '\n') != NULL || strchr(str, '\r') != NULL);
}

static ssize_t edit_menu_line_count(const char* str) {
    if (str == NULL || *str == '\0') {
        return 1;
    }

    ssize_t lines = 1;
    for (const char* p = str; *p != '\0'; ++p) {
        if (*p == '\n') {
            lines++;
        } else if (*p == '\r') {
            lines++;
            if (p[1] == '\n') {
                ++p;
            }
        }
    }
    return lines;
}

static ssize_t edit_menu_line_prefix_len(const char* display, ssize_t max_rows) {
    if (display == NULL || max_rows < 1) {
        return 0;
    }

    const char* end = display;
    ssize_t row = 1;
    while (*end != '\0') {
        if (*end != '\n' && *end != '\r') {
            ++end;
            continue;
        }
        if (row >= max_rows) {
            break;
        }
        row++;
        if (*end == '\r' && end[1] == '\n') {
            ++end;
        }
        ++end;
    }
    return (ssize_t)(end - display);
}

static ssize_t edit_menu_multiline_preview_row_count(ic_env_t* env, const char* display) {
    if (env == NULL || display == NULL || *display == '\0') {
        return 1;
    }

    stringbuf_t* preview = sbuf_new(env->mem);
    if (preview == NULL) {
        return edit_menu_line_count(display);
    }

    sbuf_replace(preview, display);
    rowcol_t rc_dummy = {0};
    ssize_t rows = sbuf_get_rc_at_pos(preview, edit_menu_content_width(env), 2, 2,
                                      env->line_wrap_marker_width, sbuf_len(preview), &rc_dummy);
    sbuf_free(preview);
    const ssize_t logical_rows = edit_menu_line_count(display);
    if (rows < logical_rows) {
        rows = logical_rows;
    }
    return (rows > 0 ? rows : 1);
}

static ssize_t edit_menu_expanded_preview_row_count(ic_env_t* env, const char* display,
                                                    const char* suffix_prefix, const char* suffix) {
    if (env == NULL || display == NULL) {
        return 1;
    }

    stringbuf_t* preview = sbuf_new(env->mem);
    if (preview == NULL) {
        return edit_menu_multiline_preview_row_count(env, display);
    }

    (void)sbuf_append(preview, display);
    if (suffix != NULL && suffix[0] != '\0') {
        if (suffix_prefix != NULL) {
            (void)sbuf_append(preview, suffix_prefix);
        }
        (void)sbuf_append(preview, suffix);
    }

    const ssize_t rows = edit_menu_multiline_preview_row_count(env, sbuf_string(preview));
    sbuf_free(preview);
    return rows;
}

static ssize_t edit_menu_multiline_preview_visible_len(ic_env_t* env, const char* display,
                                                       ssize_t max_rows, bool* truncated) {
    const ssize_t display_len = (display == NULL ? 0 : ic_strlen(display));
    if (truncated != NULL) {
        *truncated = false;
    }
    if (env == NULL || display == NULL || display_len <= 0) {
        return 0;
    }
    if (max_rows < 1) {
        max_rows = 1;
    }

    stringbuf_t* preview = sbuf_new(env->mem);
    if (preview == NULL) {
        const ssize_t visible_len = edit_menu_line_prefix_len(display, max_rows);
        if (truncated != NULL) {
            *truncated = (visible_len < display_len);
        }
        return visible_len;
    }

    sbuf_replace(preview, display);
    rowcol_t rc_dummy = {0};
    const ssize_t term_width = edit_menu_content_width(env);
    const ssize_t rendered_rows = sbuf_get_rc_at_pos(
        preview, term_width, 2, 2, env->line_wrap_marker_width, display_len, &rc_dummy);
    const ssize_t logical_rows = edit_menu_line_count(display);
    if (rendered_rows <= max_rows && logical_rows <= max_rows) {
        sbuf_free(preview);
        return display_len;
    }

    ssize_t visible_len = display_len;
    if (logical_rows > max_rows) {
        visible_len = edit_menu_line_prefix_len(display, max_rows);
    }
    ssize_t last_row_columns = term_width - 2 - 3;  // continuation indent and ellipsis
    if (last_row_columns < 0) {
        last_row_columns = 0;
    }
    const ssize_t wrapped_visible_len =
        (rendered_rows > max_rows
             ? sbuf_get_pos_at_rc(preview, term_width, 2, 2, env->line_wrap_marker_width,
                                  max_rows - 1, last_row_columns)
             : display_len);
    sbuf_free(preview);
    if (rendered_rows > max_rows) {
        if (wrapped_visible_len < 0) {
            visible_len = 0;
        } else if (wrapped_visible_len < visible_len) {
            visible_len = wrapped_visible_len;
        }
    }
    while (visible_len > 0 &&
           (display[visible_len - 1] == '\n' || display[visible_len - 1] == '\r')) {
        visible_len--;
    }
    if (truncated != NULL) {
        *truncated = (visible_len < display_len);
    }
    return visible_len;
}

static void edit_menu_append_indented_newlines(stringbuf_t* sb, const char* text, ssize_t len) {
    if (sb == NULL || text == NULL || len <= 0) {
        return;
    }

    const char* segment = text;
    const char* end = text + len;
    for (const char* p = text; p < end; ++p) {
        if (*p != '\n' && *p != '\r') {
            continue;
        }
        if (p > segment) {
            (void)sbuf_append_n(sb, segment, p - segment);
        }
        (void)sbuf_append(sb, "\n  ");
        if (*p == '\r' && p + 1 < end && p[1] == '\n') {
            ++p;
        }
        segment = p + 1;
    }
    if (segment < end) {
        (void)sbuf_append_n(sb, segment, end - segment);
    }
}

static bool edit_menu_append_multiline_preview(ic_env_t* env, editor_t* eb, const char* display,
                                               bool syntax_highlight, bool parse_bbcode,
                                               ssize_t max_rows, const char* suffix_prefix,
                                               const char* suffix) {
    if (env == NULL || eb == NULL || display == NULL) {
        return false;
    }

    stringbuf_t* combined = sbuf_new(env->mem);
    if (combined == NULL) {
        suffix_prefix = NULL;
        suffix = NULL;
    } else {
        (void)sbuf_append(combined, display);
        if (suffix != NULL && suffix[0] != '\0') {
            if (suffix_prefix != NULL) {
                (void)sbuf_append(combined, suffix_prefix);
            }
            (void)sbuf_append(combined, suffix);
        }
    }

    const char* preview_text = (combined == NULL ? display : sbuf_string(combined));
    bool truncated = false;
    const ssize_t visible_len =
        edit_menu_multiline_preview_visible_len(env, preview_text, max_rows, &truncated);
    const ssize_t display_len = ic_strlen(display);
    const ssize_t display_visible_len = (visible_len < display_len ? visible_len : display_len);
    ssize_t remaining = visible_len - display_visible_len;
    const ssize_t suffix_prefix_len =
        (suffix_prefix == NULL || suffix == NULL || suffix[0] == '\0' ? 0
                                                                      : ic_strlen(suffix_prefix));
    const ssize_t suffix_prefix_visible_len =
        (remaining < suffix_prefix_len ? remaining : suffix_prefix_len);
    remaining -= suffix_prefix_visible_len;
    const ssize_t suffix_len = (suffix == NULL ? 0 : ic_strlen(suffix));
    const ssize_t suffix_visible_len = (remaining < suffix_len ? remaining : suffix_len);
    const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
    if (syntax_highlight) {
        (void)sbuf_append(eb->extra, "[ic-menu-selected][!pre]");
        (void)sbuf_appendf(eb->extra, "%s ", arrow);
        (void)sbuf_append(eb->extra, "[/pre]");
        edit_menu_append_syntax_highlighted_text(env, eb->extra, display, display_visible_len,
                                                 parse_bbcode, -1, 0, true, false, "  ");
    } else {
        (void)sbuf_append(eb->extra, "[ic-menu-selected][!pre]");
        (void)sbuf_appendf(eb->extra, "%s ", arrow);
        edit_menu_append_indented_newlines(eb->extra, display, display_visible_len);
        (void)sbuf_append(eb->extra, "[/pre]");
    }
    if (suffix_prefix_visible_len > 0) {
        edit_menu_append_indented_newlines(eb->extra, suffix_prefix, suffix_prefix_visible_len);
    }
    if (suffix_visible_len > 0) {
        (void)sbuf_appendf(eb->extra, "[%s]", edit_menu_tag_style(true));
        edit_menu_append_escaped_n(eb->extra, suffix, suffix_visible_len);
        (void)sbuf_append(eb->extra, "[/]");
    }
    if (truncated) {
        (void)sbuf_append(eb->extra, "...");
    }
    (void)sbuf_append(eb->extra, "[/ic-menu-selected]");
    sbuf_free(combined);
    return truncated;
}

static ssize_t edit_menu_visible_prefix(const char* s, ssize_t len, ssize_t max_columns,
                                        ssize_t* width_out) {
    if (s == NULL || len <= 0 || max_columns <= 0) {
        if (width_out != NULL) {
            *width_out = 0;
        }
        return 0;
    }

    ssize_t pos = 0;
    ssize_t width = 0;
    while (pos < len) {
        ssize_t cw = 0;
        ssize_t next = str_next_ofs(s, len, pos, &cw);
        if (next <= 0) {
            break;
        }

        if (cw <= 0) {
            pos += next;
            continue;
        }

        if (width + cw > max_columns) {
            break;
        }

        width += cw;
        pos += next;
    }

    if (width_out != NULL) {
        *width_out = width;
    }
    return pos;
}

typedef struct edit_menu_preview_s {
    ssize_t entry_len;
    ssize_t visible_len;
    bool append_ellipsis;
} edit_menu_preview_t;

static edit_menu_preview_t edit_menu_preview(const char* display, ssize_t max_columns) {
    const char* line_end = edit_menu_first_line_end(display);
    const ssize_t entry_len = line_end ? line_end - display : ic_strlen(display);
    const bool multiline = line_end != NULL && (*line_end == '\n' || *line_end == '\r');
    if (max_columns < 4) {
        max_columns = 4;
    }

    ssize_t width = 0;
    ssize_t visible_len = edit_menu_visible_prefix(display, entry_len, max_columns, &width);
    const bool ellipsis = multiline || visible_len < entry_len;
    if (ellipsis && width + 3 > max_columns) {
        visible_len = edit_menu_visible_prefix(display, entry_len, max_columns - 3, NULL);
    }
    return (edit_menu_preview_t){entry_len, visible_len, ellipsis};
}

static void edit_menu_append_highlighted_prefix(stringbuf_t* sb, const char* display,
                                                ssize_t visible_len, ssize_t entry_len,
                                                ssize_t match_pos, ssize_t match_len, bool selected,
                                                bool highlight, ic_env_t* env,
                                                bool syntax_highlight) {
    if (sb == NULL || display == NULL || visible_len <= 0) {
        return;
    }

    if (syntax_highlight && env != NULL) {
        edit_menu_append_syntax_highlighted_text(env, sb, display, visible_len, false, match_pos,
                                                 match_len, selected, highlight, NULL);
        return;
    }

    if (highlight && match_len > 0 && match_pos >= 0) {
        if (match_pos < 0) {
            match_pos = 0;
        }
        if (match_pos < visible_len) {
            if (match_pos > 0) {
                ssize_t prefix_len = (match_pos <= visible_len ? match_pos : visible_len);
                (void)sbuf_append_n(sb, display, prefix_len);
            }

            if (match_len > 0) {
                if (match_pos + match_len > entry_len) {
                    match_len = entry_len - match_pos;
                }
                if (match_pos + match_len > visible_len) {
                    match_len = visible_len - match_pos;
                }
            }

            if (match_len > 0) {
                if (selected) {
                    (void)sbuf_append(sb, "[/pre][u][!pre]");
                } else {
                    (void)sbuf_append(sb, "[/pre][u ic-emphasis][!pre]");
                }
                (void)sbuf_append_n(sb, display + match_pos, match_len);
                (void)sbuf_append(sb, "[/pre][/u][!pre]");
            }

            ssize_t suffix_start = match_pos + match_len;
            if (suffix_start < visible_len) {
                (void)sbuf_append_n(sb, display + suffix_start, visible_len - suffix_start);
            }
            return;
        }
    }

    (void)sbuf_append_n(sb, display, visible_len);
}

static void edit_menu_append_scroll_hint(stringbuf_t* sb, ssize_t item_count, ssize_t display_count,
                                         ssize_t scroll_offset) {
    if (sb == NULL || item_count <= display_count) {
        return;
    }

    ssize_t hidden_above = scroll_offset;
    ssize_t hidden_below = item_count - (scroll_offset + display_count);
    if (hidden_above > 0 && hidden_below > 0) {
        (void)sbuf_appendf(sb, "[ic-info]  (%zd above, %zd below)[/]\n", hidden_above,
                           hidden_below);
    } else if (hidden_above > 0) {
        (void)sbuf_appendf(sb, "[ic-info]  (%zd more above)[/]\n", hidden_above);
    } else if (hidden_below > 0) {
        (void)sbuf_appendf(sb, "[ic-info]  (%zd more below)[/]\n", hidden_below);
    }
}

static bool edit_menu_mouse_select_vertical(ic_env_t* env, editor_t* eb, ssize_t item_count,
                                            ssize_t scroll_offset, ssize_t display_count,
                                            ssize_t status_rows, ssize_t* selected_idx,
                                            bool* accept_selection) {
    if (env == NULL || eb == NULL || env->tty == NULL || selected_idx == NULL ||
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

    if (item_count <= 0 || display_count <= 0) {
        return false;
    }

    ssize_t target_row = 0;
    ssize_t target_col = 0;
    if (!edit_mouse_event_to_target_rowcol(env, eb, &mouse_event, &target_row, &target_col, NULL)) {
        return false;
    }
    ic_unused(target_col);

    const ssize_t input_rows = (eb->input_rows > 0 ? eb->input_rows : 1);
    const ssize_t items_first_row = input_rows + status_rows;
    const ssize_t item_row = target_row - items_first_row;
    if (item_row < 0 || item_row >= display_count) {
        return false;
    }

    const ssize_t idx = scroll_offset + item_row;
    if (idx < 0 || idx >= item_count) {
        return false;
    }

    *selected_idx = idx;
    *accept_selection = (mouse_event.action == TTY_MOUSE_ACTION_LEFT_RELEASE);
    return true;
}

static bool edit_menu_mouse_event_is_left_click(ic_env_t* env) {
    if (env == NULL || env->tty == NULL) {
        return false;
    }

    tty_mouse_event_t mouse_event;
    if (!tty_get_last_mouse_event(env->tty, &mouse_event)) {
        return false;
    }
    return (mouse_event.action == TTY_MOUSE_ACTION_LEFT_PRESS ||
            mouse_event.action == TTY_MOUSE_ACTION_LEFT_RELEASE);
}

// Shared menu input distinguishes changes to the search from navigation/redraws.
typedef enum edit_menu_input_e {
    EDIT_MENU_INPUT_UNHANDLED,
    EDIT_MENU_INPUT_SELECTION,
    EDIT_MENU_INPUT_QUERY,
    EDIT_MENU_INPUT_CASE
} edit_menu_input_t;

static bool edit_menu_read_event(ic_env_t* env, editor_t* eb, edit_menu_session_t* session,
                                 code_t* key, ssize_t* scroll_offset, ssize_t* selected) {
    *key = KEY_ESC;
    (void)edit_menu_read_key(env, eb, key);
    if (*key == KEY_EVENT_RESIZE || tty_term_resize_event(env->tty)) {
        edit_menu_scrollbar_release(env, eb, &session->scrollbar);
        (void)edit_resize(env, eb);
    }
    sbuf_clear(eb->extra);
    if (edit_menu_scrollbar_event(env, eb, &session->scrollbar, *key, session->mouse_scroll_enabled,
                                  scroll_offset, selected)) {
        return false;
    }
    if (edit_menu_mouse_prepare_key(env, eb, *key, true, &session->mouse_scroll_enabled,
                                    &session->mouse_suspended)) {
        return false;
    }
    if (*key == KEY_LINEFEED) {
        session->maximized = !session->maximized;
        return false;  // redraw without changing the query or selection
    }
    return true;
}

static edit_menu_input_t edit_menu_handle_input(ic_env_t* env, editor_t* eb, code_t key,
                                                const edit_menu_session_t* session, ssize_t count,
                                                ssize_t display_count, ssize_t max_scroll,
                                                ssize_t* scroll_offset, ssize_t* selected,
                                                bool* case_sensitive, bool allow_live_input) {
    const code_t plain = KEY_NO_MODS(key);
    if ((KEY_MODS(key) & KEY_MOD_SHIFT) && plain == KEY_DOWN) {
        (void)edit_menu_page_down(env, count, display_count, max_scroll, scroll_offset, selected);
    } else if ((KEY_MODS(key) & KEY_MOD_SHIFT) && plain == KEY_UP) {
        (void)edit_menu_page_up(env, count, display_count, scroll_offset, selected);
    } else if ((KEY_MODS(key) & KEY_MOD_ALT) && (plain == 'c' || plain == 'C')) {
        *case_sensitive = !*case_sensitive;
        return EDIT_MENU_INPUT_CASE;
    } else if (plain == KEY_UP || key == KEY_CTRL_P ||
               (session->mouse_scroll_enabled && plain == KEY_EVENT_MOUSE_WHEEL_UP)) {
        if (allow_live_input && *selected == 0) {
            *selected = -1;
        } else {
            (void)edit_menu_move_selection(env, count, -1, selected);
        }
    } else if (plain == KEY_DOWN || key == KEY_CTRL_N ||
               (session->mouse_scroll_enabled && plain == KEY_EVENT_MOUSE_WHEEL_DOWN)) {
        (void)edit_menu_move_selection(env, count, 1, selected);
    } else if (key == KEY_BACKSP) {
        if (eb->pos > 0) {
            edit_backspace(env, eb);
            return EDIT_MENU_INPUT_QUERY;
        }
    } else if (key == KEY_DEL) {
        edit_delete_char(env, eb);
        return EDIT_MENU_INPUT_QUERY;
    } else if (key == KEY_F1) {
        edit_show_help(env, eb);
    } else {
        char chr;
        unicode_t uchr;
        if (code_is_ascii_char(key, &chr)) {
            edit_insert_char(env, eb, chr);
        } else if (code_is_unicode(key, &uchr)) {
            edit_insert_unicode(env, eb, uchr);
        } else {
            return EDIT_MENU_INPUT_UNHANDLED;
        }
        return EDIT_MENU_INPUT_QUERY;
    }
    return EDIT_MENU_INPUT_SELECTION;
}
