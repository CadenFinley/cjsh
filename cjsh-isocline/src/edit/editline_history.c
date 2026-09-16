/*
  editline_history.c

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
// History search: this file is included in editline.c
//-------------------------------------------------------------

// Helper function to clear history preview
static void edit_clear_history_preview(editor_t* eb) {
    if (eb == NULL) {
        return;
    }
    if (sbuf_len(eb->extra) > 0) {
        sbuf_clear(eb->extra);
    }
    if (eb->history_prefix != NULL) {
        sbuf_clear(eb->history_prefix);
    }
    eb->history_prefix_active = false;
}

static bool history_search_extract_preview_key(const char* query, char* key_buf,
                                               size_t key_buf_size) {
    if (key_buf == NULL || key_buf_size == 0) {
        return false;
    }
    key_buf[0] = '\0';
    if (query == NULL || query[0] == '\0') {
        return false;
    }

    bool found = false;
    const char* cursor = query;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        const char* token_start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }

        size_t token_len = (size_t)(cursor - token_start);
        if (token_len < 3) {
            continue;
        }

        for (size_t i = 1; i + 1 < token_len; ++i) {
            if (token_start[i] == ':' && token_start[i + 1] == ':' && i + 2 == token_len) {
                size_t key_len = i;
                if (key_len >= key_buf_size) {
                    key_len = key_buf_size - 1;
                }
                ic_memcpy(key_buf, token_start, (ssize_t)key_len);
                key_buf[key_len] = '\0';
                found = (key_len > 0);
            }
        }
    }

    return found;
}

static bool history_search_has_valid_metadata_tag(const char* query) {
    if (query == NULL || query[0] == '\0') {
        return false;
    }

    const char* cursor = query;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        const char* token_start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }

        size_t token_len = (size_t)(cursor - token_start);
        if (token_len < 3) {
            continue;
        }

        for (size_t i = 1; i + 1 < token_len; ++i) {
            if (token_start[i] == ':' && token_start[i + 1] == ':') {
                bool valid_key = true;
                for (size_t j = 0; j < i; ++j) {
                    char c = token_start[j];
                    if (c == ' ' || c == '\t' || c == '=') {
                        valid_key = false;
                        break;
                    }
                }
                if (valid_key) {
                    return true;
                }
            }
        }
    }

    return false;
}

static const char* k_history_search_timestamp_key = "timestamp";
static const char* k_history_search_exit_code_key = "code";
static const char* k_history_search_exit_code_legacy_key = "exit_code";

static const char* history_search_entry_exit_code(const history_entry_t* entry) {
    if (entry == NULL) {
        return NULL;
    }

    const char* exit_code = history_entry_get_metadata(entry, k_history_search_exit_code_key);
    if (exit_code == NULL || exit_code[0] == '\0') {
        exit_code = history_entry_get_metadata(entry, k_history_search_exit_code_legacy_key);
    }

    if (exit_code == NULL || exit_code[0] == '\0') {
        return NULL;
    }
    return exit_code;
}

static bool history_search_format_relative_time(time_t timestamp, char* formatted_buf,
                                                size_t formatted_buf_size) {
    if (formatted_buf == NULL || formatted_buf_size == 0) {
        return false;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return false;
    }

    long long now_ll = (long long)now;
    long long ts_ll = (long long)timestamp;
    bool in_future = (ts_ll > now_ll);
    unsigned long long delta_seconds =
        (unsigned long long)(in_future ? (ts_ll - now_ll) : (now_ll - ts_ll));

    if (delta_seconds < 5ULL) {
        int n = snprintf(formatted_buf, formatted_buf_size, "%s",
                         in_future ? "in a moment" : "just now");
        return (n > 0 && (size_t)n < formatted_buf_size);
    }

    unsigned long long amount = delta_seconds;
    const char* unit = "s";

    if (delta_seconds >= 31557600ULL) {
        amount = delta_seconds / 31557600ULL;
        unit = "y";
    } else if (delta_seconds >= 2629800ULL) {
        amount = delta_seconds / 2629800ULL;
        unit = "mo";
    } else if (delta_seconds >= 604800ULL) {
        amount = delta_seconds / 604800ULL;
        unit = "w";
    } else if (delta_seconds >= 86400ULL) {
        amount = delta_seconds / 86400ULL;
        unit = "d";
    } else if (delta_seconds >= 3600ULL) {
        amount = delta_seconds / 3600ULL;
        unit = "h";
    } else if (delta_seconds >= 60ULL) {
        amount = delta_seconds / 60ULL;
        unit = "m";
    }

    int n = snprintf(formatted_buf, formatted_buf_size, in_future ? "in %llu%s" : "%llu%s ago",
                     amount, unit);
    return (n > 0 && (size_t)n < formatted_buf_size);
}

static const char* history_search_pretty_metadata_value(const char* key, const char* value,
                                                        char* formatted_buf,
                                                        size_t formatted_buf_size) {
    if (value == NULL || value[0] == '\0') {
        return "-";
    }

    if (key == NULL || ic_stricmp(key, k_history_search_timestamp_key) != 0) {
        return value;
    }

    char* end = NULL;
    long long epoch = strtoll(value, &end, 10);
    if (end == value || end == NULL || *end != '\0' || epoch < 0) {
        return value;
    }

    // Only treat values in a plausible unix-seconds range as timestamps.
    if (epoch < 946684800LL || epoch > 4102444800LL) {
        return value;
    }

    time_t ts = (time_t)epoch;
    if ((long long)ts != epoch) {
        return value;
    }

    if (!history_search_format_relative_time(ts, formatted_buf, formatted_buf_size)) {
        return value;
    }

    return formatted_buf;
}

static bool history_search_sort_is_metadata(ic_history_search_sort_t sort) {
    return (sort == IC_HISTORY_SEARCH_SORT_METADATA_ASC ||
            sort == IC_HISTORY_SEARCH_SORT_METADATA_DESC);
}

static const char* history_search_entry_metadata_value(const history_entry_t* entry,
                                                       const char* key) {
    if (entry == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    if (ic_stricmp(key, k_history_search_exit_code_key) == 0 ||
        ic_stricmp(key, k_history_search_exit_code_legacy_key) == 0) {
        return history_search_entry_exit_code(entry);
    }

    return history_entry_get_metadata(entry, key);
}

static void history_search_build_compact_metadata_suffix(stringbuf_t* suffix,
                                                         const history_entry_t* entry,
                                                         const char* metadata_key,
                                                         bool use_default_tag) {
    if (suffix == NULL) {
        return;
    }
    sbuf_clear(suffix);
    if (entry == NULL) {
        return;
    }

    char formatted_meta_value[64];
    if (use_default_tag) {
        const char* exit_code = history_search_entry_exit_code(entry);
        const char* timestamp = history_entry_get_metadata(entry, k_history_search_timestamp_key);
        const char* shown_value = history_search_pretty_metadata_value(
            k_history_search_timestamp_key, timestamp, formatted_meta_value,
            sizeof(formatted_meta_value));
        if (exit_code != NULL) {
            (void)sbuf_appendf(suffix, " [%s | %s]", shown_value, exit_code);
        } else if (timestamp != NULL && timestamp[0] != '\0') {
            (void)sbuf_appendf(suffix, " [%s]", shown_value);
        }
        return;
    }

    if (metadata_key == NULL || metadata_key[0] == '\0') {
        return;
    }
    const char* meta_value = history_search_entry_metadata_value(entry, metadata_key);
    const char* shown_value = history_search_pretty_metadata_value(
        metadata_key, meta_value, formatted_meta_value, sizeof(formatted_meta_value));
    (void)sbuf_appendf(suffix, " [%s]", shown_value);
}

static void history_search_append_metadata_label(stringbuf_t* sb, const char* key) {
    if (sb == NULL || key == NULL || key[0] == '\0') {
        return;
    }
    if (ic_stricmp(key, k_history_search_timestamp_key) == 0) {
        (void)sbuf_append(sb, "Time");
        return;
    }
    if (ic_stricmp(key, k_history_search_exit_code_key) == 0 ||
        ic_stricmp(key, k_history_search_exit_code_legacy_key) == 0) {
        (void)sbuf_append(sb, "Exit code");
        return;
    }
    if (ic_stricmp(key, "frequency") == 0) {
        (void)sbuf_append(sb, "Frequency");
        return;
    }

    bool capitalize = true;
    for (const char* p = key; *p != '\0'; ++p) {
        char c = *p;
        if (c == '_' || c == '-') {
            (void)sbuf_append_char(sb, ' ');
            capitalize = true;
        } else {
            if (capitalize && c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            (void)sbuf_append_char(sb, c);
            capitalize = false;
        }
    }
}

static void history_search_append_inline_metadata_value(stringbuf_t* sb, const char* value) {
    if (sb == NULL || value == NULL) {
        return;
    }
    const char* segment = value;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p != '\n' && *p != '\r') {
            continue;
        }
        if (p > segment) {
            (void)sbuf_append_n(sb, segment, p - segment);
        }
        (void)sbuf_append_char(sb, ' ');
        if (*p == '\r' && p[1] == '\n') {
            ++p;
        }
        segment = p + 1;
    }
    if (*segment != '\0') {
        (void)sbuf_append(sb, segment);
    }
}

static void history_search_build_expanded_metadata(stringbuf_t* suffix,
                                                   const history_entry_t* entry) {
    if (suffix == NULL) {
        return;
    }
    sbuf_clear(suffix);
    if (entry == NULL || entry->metadata == NULL || entry->metadata_count <= 0) {
        return;
    }

    bool first = true;
    (void)sbuf_append(suffix, " [");
    for (ssize_t i = 0; i < entry->metadata_count; ++i) {
        const char* key = entry->metadata[i].key;
        if (key == NULL || key[0] == '\0') {
            continue;
        }
        const char* value = entry->metadata[i].value;
        char formatted_meta_value[64];
        const char* shown_value = history_search_pretty_metadata_value(
            key, value, formatted_meta_value, sizeof(formatted_meta_value));
        if (!first) {
            (void)sbuf_append(suffix, " | ");
        }
        history_search_append_metadata_label(suffix, key);
        (void)sbuf_append(suffix, ": ");
        history_search_append_inline_metadata_value(suffix, shown_value);
        first = false;
    }
    if (first) {
        sbuf_clear(suffix);
    } else {
        (void)sbuf_append_char(suffix, ']');
    }
}

static bool history_search_parse_integer(const char* value, long long* value_out) {
    if (value_out != NULL) {
        *value_out = 0;
    }
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    char* end = NULL;
    long long parsed = strtoll(value, &end, 10);
    if (end == value || end == NULL || *end != '\0') {
        return false;
    }

    if (value_out != NULL) {
        *value_out = parsed;
    }
    return true;
}

static int history_search_compare_text(const char* left, const char* right) {
    if (left == NULL) {
        left = "";
    }
    if (right == NULL) {
        right = "";
    }

    const char* l = left;
    const char* r = right;
    while (*l != '\0' && *r != '\0') {
        char lc = ic_tolower(*l);
        char rc = ic_tolower(*r);
        if (lc < rc) {
            return -1;
        }
        if (lc > rc) {
            return 1;
        }
        l++;
        r++;
    }

    if (*l == '\0' && *r != '\0') {
        return -1;
    }
    if (*l != '\0' && *r == '\0') {
        return 1;
    }

    int exact = strcmp(left, right);
    if (exact < 0) {
        return -1;
    }
    if (exact > 0) {
        return 1;
    }
    return 0;
}

static int history_search_compare_recent(const history_match_t* left,
                                         const history_match_t* right) {
    if (left == NULL || right == NULL) {
        return 0;
    }
    if (left->hidx < right->hidx) {
        return -1;
    }
    if (left->hidx > right->hidx) {
        return 1;
    }
    return 0;
}

static int history_search_compare_metadata(const history_entry_t* left,
                                           const history_entry_t* right, const char* key,
                                           bool descending) {
    const char* left_value = history_search_entry_metadata_value(left, key);
    const char* right_value = history_search_entry_metadata_value(right, key);
    bool left_present = (left_value != NULL);
    bool right_present = (right_value != NULL);

    if (!left_present && !right_present) {
        return 0;
    }
    if (!left_present) {
        return 1;
    }
    if (!right_present) {
        return -1;
    }

    long long left_number = 0;
    long long right_number = 0;
    bool left_numeric = history_search_parse_integer(left_value, &left_number);
    bool right_numeric = history_search_parse_integer(right_value, &right_number);

    int cmp = 0;
    if (left_numeric && right_numeric) {
        if (left_number < right_number) {
            cmp = -1;
        } else if (left_number > right_number) {
            cmp = 1;
        }
    } else {
        cmp = history_search_compare_text(left_value, right_value);
    }

    return descending ? -cmp : cmp;
}

typedef struct history_search_match_sort_context_s {
    const history_snapshot_t* snap;
    ic_history_search_sort_t sort;
    const char* metadata_key;
} history_search_match_sort_context_t;

static const history_search_match_sort_context_t* g_history_search_match_sort_context = NULL;

static int history_search_compare_matches_for_sort(const void* left_ptr, const void* right_ptr) {
    const history_match_t* left = (const history_match_t*)left_ptr;
    const history_match_t* right = (const history_match_t*)right_ptr;
    const history_search_match_sort_context_t* ctx = g_history_search_match_sort_context;

    if (ctx == NULL || left == NULL || right == NULL) {
        return 0;
    }

    const history_entry_t* left_entry = history_snapshot_get(ctx->snap, left->hidx);
    const history_entry_t* right_entry = history_snapshot_get(ctx->snap, right->hidx);
    int cmp = 0;

    switch (ctx->sort) {
        case IC_HISTORY_SEARCH_SORT_COMMAND_ASC:
        case IC_HISTORY_SEARCH_SORT_COMMAND_DESC:
            cmp = history_search_compare_text(left_entry != NULL ? left_entry->command : NULL,
                                              right_entry != NULL ? right_entry->command : NULL);
            if (ctx->sort == IC_HISTORY_SEARCH_SORT_COMMAND_DESC) {
                cmp = -cmp;
            }
            break;
        case IC_HISTORY_SEARCH_SORT_METADATA_ASC:
        case IC_HISTORY_SEARCH_SORT_METADATA_DESC:
            if (ctx->metadata_key != NULL && ctx->metadata_key[0] != '\0') {
                cmp = history_search_compare_metadata(
                    left_entry, right_entry, ctx->metadata_key,
                    ctx->sort == IC_HISTORY_SEARCH_SORT_METADATA_DESC);
            }
            break;
        case IC_HISTORY_SEARCH_SORT_RECENT:
        default:
            break;
    }

    if (cmp != 0) {
        return cmp;
    }
    return history_search_compare_recent(left, right);
}

static void history_search_sort_matches(const history_snapshot_t* snap, history_match_t* matches,
                                        ssize_t match_count, ic_history_search_sort_t sort,
                                        const char* metadata_key) {
    if (snap == NULL || matches == NULL || match_count <= 1) {
        return;
    }
    if (history_search_sort_is_metadata(sort) &&
        (metadata_key == NULL || metadata_key[0] == '\0')) {
        sort = IC_HISTORY_SEARCH_SORT_RECENT;
    }

    history_search_match_sort_context_t ctx = {
        .snap = snap,
        .sort = sort,
        .metadata_key = metadata_key,
    };
    const history_search_match_sort_context_t* old_ctx = g_history_search_match_sort_context;
    g_history_search_match_sort_context = &ctx;
    qsort(matches, (size_t)match_count, sizeof(history_match_t),
          history_search_compare_matches_for_sort);
    g_history_search_match_sort_context = old_ctx;
}

// The newest history entry is scratch space for the live editor buffer. Keep it available for
// normal history navigation, but do not present it as a fuzzy-search result.
static void history_search_remove_scratch_match(history_match_t* matches, ssize_t* match_count) {
    if (matches == NULL || match_count == NULL || *match_count <= 0) {
        return;
    }

    ssize_t write_idx = 0;
    for (ssize_t read_idx = 0; read_idx < *match_count; ++read_idx) {
        if (matches[read_idx].hidx == 0) {
            continue;
        }
        if (write_idx != read_idx) {
            matches[write_idx] = matches[read_idx];
        }
        write_idx++;
    }
    *match_count = write_idx;
}

static void history_search_append_empty_selection(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || eb->extra == NULL) {
        return;
    }

    const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
    (void)sbuf_appendf(eb->extra, "[ic-menu-selected][!pre]%s [/pre][/ic-menu-selected]\n", arrow);
}

static const char* history_search_sort_label(ic_history_search_sort_t sort,
                                             const char* metadata_key, char* label_buf,
                                             size_t label_buf_size) {
    if (label_buf == NULL || label_buf_size == 0) {
        return "";
    }

    switch (sort) {
        case IC_HISTORY_SEARCH_SORT_COMMAND_ASC:
            return "command A-Z";
        case IC_HISTORY_SEARCH_SORT_COMMAND_DESC:
            return "command Z-A";
        case IC_HISTORY_SEARCH_SORT_METADATA_ASC:
        case IC_HISTORY_SEARCH_SORT_METADATA_DESC: {
            const char* key =
                (metadata_key != NULL && metadata_key[0] != '\0') ? metadata_key : "?";
            int n = snprintf(label_buf, label_buf_size, "%s %s", key,
                             sort == IC_HISTORY_SEARCH_SORT_METADATA_ASC ? "asc" : "desc");
            if (n < 0) {
                label_buf[0] = '\0';
            } else if ((size_t)n >= label_buf_size) {
                label_buf[label_buf_size - 1] = '\0';
            }
            return label_buf;
        }
        case IC_HISTORY_SEARCH_SORT_RECENT:
        default:
            return "recent";
    }
}

typedef struct history_search_sort_choice_s {
    ic_history_search_sort_t sort;
    const char* metadata_key;
} history_search_sort_choice_t;

static bool history_search_sort_choice_matches(const history_search_sort_choice_t* choice,
                                               ic_history_search_sort_t sort,
                                               const char* metadata_key) {
    if (choice == NULL || choice->sort != sort) {
        return false;
    }
    if (!history_search_sort_is_metadata(sort)) {
        return true;
    }
    if (choice->metadata_key == NULL || metadata_key == NULL) {
        return choice->metadata_key == metadata_key;
    }
    return ic_stricmp(choice->metadata_key, metadata_key) == 0;
}

static bool history_search_sort_choice_key_seen(const history_search_sort_choice_t* choices,
                                                ssize_t choice_count, const char* key) {
    if (choices == NULL || key == NULL) {
        return false;
    }
    for (ssize_t i = 0; i < choice_count; ++i) {
        if (history_search_sort_is_metadata(choices[i].sort) && choices[i].metadata_key != NULL &&
            ic_stricmp(choices[i].metadata_key, key) == 0) {
            return true;
        }
    }
    return false;
}

static bool history_search_sort_choices_append(ic_env_t* env,
                                               history_search_sort_choice_t** choices,
                                               ssize_t* choice_count, ssize_t* choice_capacity,
                                               ic_history_search_sort_t sort,
                                               const char* metadata_key) {
    if (env == NULL || choices == NULL || choice_count == NULL || choice_capacity == NULL) {
        return false;
    }
    if (*choice_count >= *choice_capacity) {
        ssize_t new_capacity = (*choice_capacity == 0 ? 8 : *choice_capacity * 2);
        history_search_sort_choice_t* resized =
            mem_realloc_tp(env->mem, history_search_sort_choice_t, *choices, new_capacity);
        if (resized == NULL) {
            return false;
        }
        *choices = resized;
        *choice_capacity = new_capacity;
    }

    (*choices)[*choice_count].sort = sort;
    (*choices)[*choice_count].metadata_key = metadata_key;
    (*choice_count)++;
    return true;
}

static bool history_search_sort_choices_append_metadata_key(ic_env_t* env,
                                                            history_search_sort_choice_t** choices,
                                                            ssize_t* choice_count,
                                                            ssize_t* choice_capacity,
                                                            const char* key) {
    if (key == NULL || key[0] == '\0') {
        return true;
    }
    if (history_search_sort_choice_key_seen(*choices, *choice_count, key)) {
        return true;
    }

    return history_search_sort_choices_append(env, choices, choice_count, choice_capacity,
                                              IC_HISTORY_SEARCH_SORT_METADATA_ASC, key) &&
           history_search_sort_choices_append(env, choices, choice_count, choice_capacity,
                                              IC_HISTORY_SEARCH_SORT_METADATA_DESC, key);
}

static bool history_search_sort_choices_add_entry_metadata(ic_env_t* env,
                                                           history_search_sort_choice_t** choices,
                                                           ssize_t* choice_count,
                                                           ssize_t* choice_capacity,
                                                           const history_entry_t* entry) {
    if (entry == NULL) {
        return true;
    }
    for (ssize_t i = 0; i < entry->metadata_count; ++i) {
        const char* key = entry->metadata[i].key;
        if (!history_search_sort_choices_append_metadata_key(env, choices, choice_count,
                                                             choice_capacity, key)) {
            return false;
        }
    }
    return true;
}

static bool history_search_set_session_sort_key(ic_env_t* env, char** sort_key,
                                                const char* metadata_key) {
    if (env == NULL || sort_key == NULL) {
        return false;
    }

    char* key_copy = NULL;
    if (metadata_key != NULL && metadata_key[0] != '\0') {
        key_copy = mem_strdup(env->mem, metadata_key);
        if (key_copy == NULL) {
            return false;
        }
    }

    mem_free(env->mem, *sort_key);
    *sort_key = key_copy;
    return true;
}

static bool history_search_cycle_sort(ic_env_t* env, const history_snapshot_t* snap,
                                      const history_match_t* matches, ssize_t match_count,
                                      ic_history_search_sort_t* sort, char** sort_key) {
    if (env == NULL || snap == NULL || sort == NULL || sort_key == NULL) {
        return false;
    }

    history_search_sort_choice_t* choices = NULL;
    ssize_t choice_count = 0;
    ssize_t choice_capacity = 0;

    bool ok = history_search_sort_choices_append(env, &choices, &choice_count, &choice_capacity,
                                                 IC_HISTORY_SEARCH_SORT_RECENT, NULL) &&
              history_search_sort_choices_append(env, &choices, &choice_count, &choice_capacity,
                                                 IC_HISTORY_SEARCH_SORT_COMMAND_ASC, NULL) &&
              history_search_sort_choices_append(env, &choices, &choice_count, &choice_capacity,
                                                 IC_HISTORY_SEARCH_SORT_COMMAND_DESC, NULL);

    if (ok) {
        if (match_count > 0 && matches != NULL) {
            for (ssize_t i = 0; i < match_count && ok; ++i) {
                const history_entry_t* entry = history_snapshot_get(snap, matches[i].hidx);
                ok = history_search_sort_choices_add_entry_metadata(env, &choices, &choice_count,
                                                                    &choice_capacity, entry);
            }
        } else {
            ssize_t total_history = history_snapshot_count(snap);
            for (ssize_t i = 0; i < total_history && ok; ++i) {
                const history_entry_t* entry = history_snapshot_get(snap, i);
                ok = history_search_sort_choices_add_entry_metadata(env, &choices, &choice_count,
                                                                    &choice_capacity, entry);
            }
        }
    }

    if (!ok || choice_count <= 0) {
        mem_free(env->mem, choices);
        return false;
    }

    ssize_t current_idx = -1;
    for (ssize_t i = 0; i < choice_count; ++i) {
        if (history_search_sort_choice_matches(&choices[i], *sort, *sort_key)) {
            current_idx = i;
            break;
        }
    }

    ssize_t next_idx = (current_idx >= 0 ? current_idx + 1 : 0);
    if (next_idx >= choice_count) {
        next_idx = 0;
    }

    history_search_sort_choice_t next = choices[next_idx];
    if (history_search_sort_is_metadata(next.sort)) {
        ok = history_search_set_session_sort_key(env, sort_key, next.metadata_key);
    } else {
        ok = history_search_set_session_sort_key(env, sort_key, NULL);
    }
    if (ok) {
        *sort = next.sort;
    }

    mem_free(env->mem, choices);
    return ok;
}

static void edit_history_at(ic_env_t* env, editor_t* eb, int ofs) {
    if (ofs == 0) {
        return;
    }

    if (eb->modified) {
        const char* current_input = sbuf_string(eb->input);
        if (eb->history_prefix != NULL) {
            sbuf_replace(eb->history_prefix, current_input != NULL ? current_input : "");
            eb->history_prefix_active = (sbuf_len(eb->history_prefix) > 0);
        } else {
            eb->history_prefix_active = false;
        }

        (void)history_update(env->history, current_input != NULL ? current_input : "");
        eb->history_idx = 0;
        eb->modified = false;
    }

    history_snapshot_t snap = (history_snapshot_t){0};
    if (!history_snapshot_load(env->history, &snap, true)) {
        term_beep(env->term);
        return;
    }

    ssize_t total_history = history_snapshot_count(&snap);
    if (total_history <= 0) {
        term_beep(env->term);
        history_snapshot_free(env->history, &snap);
        return;
    }

    ssize_t steps = (ofs > 0) ? ofs : -ofs;
    int direction = (ofs > 0) ? 1 : -1;

    const char* prefix = NULL;
    ssize_t prefix_len = 0;
    if (eb->history_prefix_active && eb->history_prefix != NULL) {
        prefix = sbuf_string(eb->history_prefix);
        if (prefix != NULL) {
            prefix_len = (ssize_t)strlen(prefix);
            if (prefix_len == 0) {
                prefix = NULL;
            }
        } else {
            prefix_len = 0;
        }
    }

    ssize_t current_idx = eb->history_idx;
    for (ssize_t step_idx = 0; step_idx < steps; ++step_idx) {
        ssize_t candidate_idx = current_idx + direction;

        if (prefix != NULL) {
            ssize_t search_idx = current_idx + direction;
            bool match_found = false;
            while (search_idx >= 0 && search_idx < total_history) {
                const history_entry_t* candidate_entry = history_snapshot_get(&snap, search_idx);
                if (candidate_entry == NULL || candidate_entry->command == NULL) {
                    break;
                }
                if (strncmp(candidate_entry->command, prefix, (size_t)prefix_len) == 0 &&
                    candidate_entry->command[prefix_len] != '\0') {
                    candidate_idx = search_idx;
                    match_found = true;
                    break;
                }
                search_idx += direction;
            }

            if (!match_found && direction > 0) {
                if (eb->history_prefix != NULL) {
                    sbuf_clear(eb->history_prefix);
                }
                eb->history_prefix_active = false;
                prefix = NULL;
                prefix_len = 0;
                eb->history_idx = 0;
                current_idx = 0;
                --step_idx;
                continue;
            }
        }

        if (candidate_idx < 0 || candidate_idx >= total_history) {
            term_beep(env->term);
            history_snapshot_free(env->history, &snap);
            return;
        }

        current_idx = candidate_idx;
    }

    const history_entry_t* entry = history_snapshot_get(&snap, current_idx);
    if (entry == NULL || entry->command == NULL) {
        term_beep(env->term);
        history_snapshot_free(env->history, &snap);
        return;
    }

    eb->history_idx = current_idx;
    sbuf_replace(eb->input, entry->command);
    eb->pos = sbuf_len(eb->input);

    sbuf_clear(eb->extra);

    edit_refresh(env, eb);
    history_snapshot_free(env->history, &snap);
}

static void edit_history_prev(ic_env_t* env, editor_t* eb) {
    edit_history_at(env, eb, 1);
}

static void edit_history_next(ic_env_t* env, editor_t* eb) {
    edit_history_at(env, eb, -1);
}

static bool history_is_word_separator(char ch) {
    switch (ch) {
        case '|':
        case '&':
        case ';':
        case '(':
        case ')':
        case '<':
        case '>':
            return true;
        default:
            return (isspace((unsigned char)ch) != 0);
    }
}

static bool history_find_last_word_bounds(const char* command, ssize_t* start_out,
                                          ssize_t* end_out) {
    if (command == NULL || start_out == NULL || end_out == NULL) {
        return false;
    }

    const ssize_t len = ic_strlen(command);
    ssize_t pos = 0;
    ssize_t last_start = -1;
    ssize_t last_end = -1;

    // Track raw token ranges so quoted history arguments are reinserted verbatim.
    while (pos < len) {
        while (pos < len && history_is_word_separator(command[pos])) {
            pos++;
        }
        if (pos >= len) {
            break;
        }

        ssize_t token_start = pos;
        char quote = 0;
        while (pos < len) {
            char ch = command[pos];
            if (quote != 0) {
                if (quote == '"' && ch == '\\' && pos + 1 < len) {
                    pos += 2;
                    continue;
                }
                pos++;
                if (ch == quote) {
                    quote = 0;
                }
                continue;
            }
            if (ch == '\\') {
                pos += (pos + 1 < len ? 2 : 1);
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                pos++;
                continue;
            }
            if (history_is_word_separator(ch)) {
                break;
            }
            pos++;
        }

        if (pos > token_start) {
            last_start = token_start;
            last_end = pos;
        }
    }

    if (last_start < 0 || last_end <= last_start) {
        return false;
    }

    *start_out = last_start;
    *end_out = last_end;
    return true;
}

static char* history_dup_last_word(alloc_t* mem, const char* command) {
    ssize_t start = 0;
    ssize_t end = 0;
    if (!history_find_last_word_bounds(command, &start, &end)) {
        return NULL;
    }
    return mem_strndup(mem, command + start, end - start);
}

static void edit_yank_last_arg(ic_env_t* env, editor_t* eb) {
    if (env == NULL || eb == NULL || env->history == NULL || eb->input == NULL) {
        return;
    }

    history_snapshot_t snap = {0};
    if (!history_snapshot_load(env->history, &snap, false)) {
        term_beep(env->term);
        return;
    }

    ssize_t start_history_idx = 1;
    if (eb->last_arg_yank_active) {
        start_history_idx = eb->last_arg_yank_history_idx + 1;
    } else if (!eb->modified && eb->history_idx > 0) {
        start_history_idx = eb->history_idx + 1;
    }

    char* last_word = NULL;
    ssize_t found_history_idx = -1;
    const ssize_t total_history = history_snapshot_count(&snap);
    for (ssize_t idx = start_history_idx; idx < total_history; ++idx) {
        const history_entry_t* entry = history_snapshot_get(&snap, idx);
        if (entry == NULL || entry->command == NULL || entry->command[0] == '\0') {
            continue;
        }
        last_word = history_dup_last_word(env->mem, entry->command);
        if (last_word != NULL && last_word[0] != '\0') {
            found_history_idx = idx;
            break;
        }
        mem_free(env->mem, last_word);
        last_word = NULL;
    }
    history_snapshot_free(env->history, &snap);

    if (last_word == NULL || found_history_idx < 0) {
        mem_free(env->mem, last_word);
        term_beep(env->term);
        return;
    }

    ssize_t replace_start = eb->pos;
    ssize_t replace_end = eb->pos;
    if (eb->last_arg_yank_active) {
        replace_start = eb->last_arg_yank_start;
        replace_end = eb->last_arg_yank_end;
    }

    ssize_t input_len = sbuf_len(eb->input);
    if (replace_start < 0) {
        replace_start = 0;
    }
    if (replace_start > input_len) {
        replace_start = input_len;
    }
    if (replace_end < replace_start) {
        replace_end = replace_start;
    }
    if (replace_end > input_len) {
        replace_end = input_len;
    }

    editor_start_modify_preserve_last_arg(eb);
    const ssize_t replaced_len = replace_end - replace_start;
    ssize_t inserted_end = sbuf_insert_at(eb->input, last_word, replace_start);
    if (last_word[0] != '\0' && inserted_end == replace_start) {
        mem_free(env->mem, last_word);
        term_beep(env->term);
        return;
    }
    if (replaced_len > 0) {
        sbuf_delete_from_to(eb->input, inserted_end, inserted_end + replaced_len);
    }

    eb->pos = inserted_end;
    eb->last_arg_yank_active = true;
    eb->last_arg_yank_history_idx = found_history_idx;
    eb->last_arg_yank_start = replace_start;
    eb->last_arg_yank_end = inserted_end;

    mem_free(env->mem, last_word);
    edit_refresh_hint(env, eb);
}

#define MAX_FUZZY_RESULTS 5000

static const char* const k_history_menu_footer =
    "[ic-diminish](↑↓/wheel:navigate shift+↑/↓:page enter:run tab:edit alt+c:case "
    "alt+d:directory alt+n:nested alt+s:sort ctrl+j:resize esc:cancel)[/]";

static void edit_history_fuzzy_search(ic_env_t* env, editor_t* eb, char* initial) {
    history_snapshot_t snap = {0};
    if (!history_snapshot_load(env->history, &snap, true)) {
        term_beep(env->term);
        return;
    }

    if (history_snapshot_count(&snap) <= 0 && !history_directory_is_enabled(env->history)) {
        term_beep(env->term);
        history_snapshot_free(env->history, &snap);
        return;
    }

    const bool original_modified = eb->modified;
    bool has_live_input = original_modified;
    if (!has_live_input) {
        const char* current_input = sbuf_string(eb->input);
        const history_entry_t* scratch_entry = history_snapshot_get(&snap, 0);
        has_live_input =
            (current_input != NULL && current_input[0] != '\0' && scratch_entry != NULL &&
             scratch_entry->command != NULL && strcmp(current_input, scratch_entry->command) == 0);
    }

    edit_menu_session_t menu_session =
        edit_menu_begin(env, eb, ic_env_get_history_search_prompt(env), true);

    history_match_t* matches =
        (history_match_t*)mem_zalloc_tp_n(env->mem, history_match_t, MAX_FUZZY_RESULTS);
    if (matches == NULL) {
        history_snapshot_free(env->history, &snap);
        term_beep(env->term);
        edit_menu_finish(env, eb, &menu_session, false, false);
        return;
    }

    ssize_t match_count = 0;
    ssize_t selected_idx = (has_live_input ? -1 : 0);
    ssize_t scroll_offset = 0;
    ssize_t last_display_count = 0;
    ssize_t last_max_scroll = 0;
    ssize_t last_status_rows = 1;
    bool session_case_sensitive = ic_history_fuzzy_search_is_case_sensitive();
    const bool original_directory = history_directory_is_enabled(env->history);
    const bool original_subdirs = history_directory_subdirs_is_enabled(env->history);
    const char* configured_sort_key = NULL;
    ic_history_search_sort_t session_sort = ic_get_history_search_sort(&configured_sort_key);
    char* session_sort_key = NULL;
    if (history_search_sort_is_metadata(session_sort) && configured_sort_key != NULL) {
        if (!history_search_set_session_sort_key(env, &session_sort_key, configured_sort_key)) {
            session_sort = IC_HISTORY_SEARCH_SORT_RECENT;
        }
    } else if (history_search_sort_is_metadata(session_sort)) {
        session_sort = IC_HISTORY_SEARCH_SORT_RECENT;
    }

    if (initial != NULL) {
        sbuf_replace(eb->input, initial);
        eb->pos = ic_strlen(initial);
    } else {
        sbuf_clear(eb->input);
        eb->pos = 0;
    }

    bool matches_dirty = true;
    bool sort_dirty = true;
    bool showing_all_due_to_no_matches = false;
    bool metadata_filter_applied = false;

again:;

    last_display_count = 0;
    last_max_scroll = 0;
    last_status_rows = 1;

    if (!history_snapshot_is_current(env->history, &snap)) {
        (void)history_snapshot_load(env->history, &snap, true);
        matches_dirty = true;
    }
    const char* scope_label = history_directory_is_enabled(env->history) ? "directory" : "all";
    const char* nested_label = history_directory_subdirs_is_enabled(env->history) ? "on" : "off";
    char metadata_preview_key[64];
    metadata_preview_key[0] = '\0';
    const char* metadata_suffix_key;
    bool metadata_suffix_use_default_tag = true;

    {
        const char* query = sbuf_string(eb->input);
        bool suppress_highlight = history_search_has_valid_metadata_tag(query);
        (void)ic_enable_highlight(suppress_highlight ? false : menu_session.old_highlight);
        (void)history_search_extract_preview_key(query, metadata_preview_key,
                                                 sizeof(metadata_preview_key));

        if (metadata_preview_key[0] != '\0') {
            metadata_suffix_key = metadata_preview_key;
            metadata_suffix_use_default_tag = false;
        } else {
            metadata_suffix_key = k_history_search_timestamp_key;
            metadata_suffix_use_default_tag = true;
        }
        if (history_search_sort_is_metadata(session_sort) && session_sort_key != NULL &&
            session_sort_key[0] != '\0') {
            metadata_suffix_key = session_sort_key;
            metadata_suffix_use_default_tag = false;
        } else if (session_sort == IC_HISTORY_SEARCH_SORT_COMMAND_ASC ||
                   session_sort == IC_HISTORY_SEARCH_SORT_COMMAND_DESC) {
            metadata_suffix_key = NULL;
            metadata_suffix_use_default_tag = false;
        }

        if (matches_dirty) {
            showing_all_due_to_no_matches = false;
            (void)history_snapshot_fuzzy_search(env->history, &snap, query ? query : "", matches,
                                                MAX_FUZZY_RESULTS, &match_count,
                                                &metadata_filter_applied, session_case_sensitive);
            if (snap.had_pending) {
                history_search_remove_scratch_match(matches, &match_count);
            }
            if (match_count == 0 && query != NULL && query[0] != '\0' && !metadata_filter_applied) {
                (void)history_snapshot_fuzzy_search(env->history, &snap, "", matches,
                                                    MAX_FUZZY_RESULTS, &match_count, NULL,
                                                    session_case_sensitive);
                if (snap.had_pending) {
                    history_search_remove_scratch_match(matches, &match_count);
                }
                showing_all_due_to_no_matches = true;
            }
            matches_dirty = false;
            sort_dirty = true;
        }
    }
    if (sort_dirty) {
        history_search_sort_matches(&snap, matches, match_count, session_sort, session_sort_key);
        sort_dirty = false;
    }

    if (selected_idx >= match_count) {
        selected_idx = match_count > 0 ? match_count - 1 : 0;
    }
    if (selected_idx < 0 && !has_live_input) {
        selected_idx = 0;
    }

    sbuf_clear(eb->extra);
    menu_session.scrollbar.rows = 0;
    const char* mouse_suffix =
        (menu_session.mouse_scroll_enabled ? " | Mouse clicking is enabled" : "");
    char sort_label_buf[128];
    const char* sort_label = history_search_sort_label(session_sort, session_sort_key,
                                                       sort_label_buf, sizeof(sort_label_buf));
    ssize_t selected_preview_rows = 0;
    bool selected_has_expanded_metadata = false;

    if (match_count > 0) {
        const char* query = sbuf_string(eb->input);
        bool is_filtered = (query != NULL && query[0] != '\0');
        stringbuf_t* selected_metadata_suffix = sbuf_new(env->mem);
        stringbuf_t* metadata_suffix_buffer = sbuf_new(env->mem);
        if (selected_idx >= 0 && selected_idx < match_count) {
            const history_entry_t* selected_entry =
                history_snapshot_get(&snap, matches[selected_idx].hidx);
            if (selected_entry != NULL && selected_entry->command != NULL) {
                history_search_build_expanded_metadata(selected_metadata_suffix, selected_entry);
                selected_has_expanded_metadata =
                    (selected_metadata_suffix != NULL && sbuf_len(selected_metadata_suffix) > 0);
                selected_preview_rows = edit_menu_expanded_preview_row_count(
                    env, selected_entry->command, NULL,
                    (selected_metadata_suffix == NULL ? NULL
                                                      : sbuf_string(selected_metadata_suffix)));
            }
        }

        if (showing_all_due_to_no_matches) {
            (void)sbuf_appendf(eb->extra,
                               "[ic-info]No matches - showing available history - "
                               "case %s - scope %s - nested %s - sort %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", scope_label,
                               nested_label, sort_label, mouse_suffix);
        } else if (is_filtered) {
            if (metadata_filter_applied) {
                (void)sbuf_appendf(eb->extra,
                                   "[ic-info]%zd match%s found (metadata filter) - case %s - scope "
                                   "%s - nested %s - sort %s%s[/]\n",
                                   match_count, match_count == 1 ? "" : "es",
                                   session_case_sensitive ? "sensitive" : "insensitive",
                                   scope_label, nested_label, sort_label, mouse_suffix);
            } else {
                (void)sbuf_appendf(
                    eb->extra,
                    "[ic-info]%zd match%s found - case %s - scope %s - nested %s - sort %s%s[/]\n",
                    match_count, match_count == 1 ? "" : "es",
                    session_case_sensitive ? "sensitive" : "insensitive", scope_label, nested_label,
                    sort_label, mouse_suffix);
            }
        } else {
            (void)sbuf_appendf(
                eb->extra,
                "[ic-info]History - case %s - scope %s - nested %s - sort %s%s[/]\n",
                session_case_sensitive ? "sensitive" : "insensitive", scope_label, nested_label,
                sort_label, mouse_suffix);
        }

        const bool show_empty_selection = (has_live_input && selected_idx < 0);
        if (show_empty_selection) {
            history_search_append_empty_selection(env, eb);
        }

        ssize_t term_width = term_get_width(env->term);
        last_status_rows = edit_menu_rendered_rows(env, eb, sbuf_string(eb->extra));
        ssize_t footer_rows =
            (!env->no_help ? edit_menu_rendered_rows(env, eb, k_history_menu_footer) : 0);
        ssize_t reserved_rows = edit_menu_input_rows(env, eb) + last_status_rows + footer_rows + 1;
        ssize_t available_lines = edit_menu_available_lines(
            env, eb, reserved_rows, 1, env->history_menu_max_line_count, menu_session.maximized);

        ssize_t rows_for_items = available_lines;
        ssize_t selected_preview_limit = 0;
        if (selected_preview_rows > 0) {
            selected_preview_limit = selected_preview_rows;
            if (selected_preview_limit > available_lines) {
                selected_preview_limit = available_lines;
            }
        }
        if (selected_preview_limit > 1) {
            rows_for_items -= (selected_preview_limit - 1);
            if (rows_for_items < 1) {
                rows_for_items = 1;
            }
        }

        edit_menu_window_t window =
            edit_menu_window_for(env, match_count, rows_for_items, selected_idx, scroll_offset);
        ssize_t display_count = window.display_count;
        scroll_offset = window.scroll_offset;

        last_display_count = display_count;
        last_max_scroll = window.max_scroll;

        const ssize_t items_start = sbuf_len(eb->extra);
        for (ssize_t i = 0; i < display_count; i++) {
            ssize_t match_idx = scroll_offset + i;
            if (match_idx >= match_count) {
                break;
            }

            const history_entry_t* entry = history_snapshot_get(&snap, matches[match_idx].hidx);
            if (entry == NULL || entry->command == NULL) {
                continue;
            }

            history_search_build_compact_metadata_suffix(metadata_suffix_buffer, entry,
                                                         metadata_suffix_key,
                                                         metadata_suffix_use_default_tag);
            const char* metadata_suffix =
                (metadata_suffix_buffer == NULL ? "" : sbuf_string(metadata_suffix_buffer));
            ssize_t metadata_reserved_columns = 0;
            ssize_t metadata_visible_len = 0;
            bool metadata_truncated = false;
            if (metadata_suffix[0] != '\0') {
                const char* metadata_line_end = edit_menu_first_line_end(metadata_suffix);
                const ssize_t metadata_len =
                    (metadata_line_end == NULL ? ic_strlen(metadata_suffix)
                                               : metadata_line_end - metadata_suffix);
                const bool metadata_multiline =
                    (metadata_line_end != NULL &&
                     (*metadata_line_end == '\n' || *metadata_line_end == '\r'));
                ssize_t max_metadata_columns = term_width - 8;
                if (max_metadata_columns < 1) {
                    max_metadata_columns = 1;
                }
                ssize_t metadata_width = 0;
                metadata_visible_len = edit_menu_visible_prefix(
                    metadata_suffix, metadata_len, max_metadata_columns, &metadata_width);
                metadata_truncated = (metadata_multiline || metadata_visible_len < metadata_len);
                if (metadata_truncated && max_metadata_columns > 3 &&
                    metadata_width + 3 > max_metadata_columns) {
                    metadata_visible_len = edit_menu_visible_prefix(
                        metadata_suffix, metadata_len, max_metadata_columns - 3, &metadata_width);
                }
                metadata_reserved_columns = metadata_width;
                if (metadata_truncated && max_metadata_columns > 3) {
                    metadata_reserved_columns += 3;
                }
            }

            const char* display = entry->command;
            const edit_menu_preview_t preview =
                edit_menu_preview(display, term_width - 4 - metadata_reserved_columns);

            bool is_selected = (match_idx == selected_idx);
            bool show_selected_expanded =
                (is_selected && (selected_has_expanded_metadata || selected_preview_rows > 1));
            bool syntax_highlight_item = edit_menu_should_syntax_highlight_item_ex(
                env, is_selected, menu_session.old_highlight);

            if (show_selected_expanded) {
                const char* expanded_metadata =
                    (selected_metadata_suffix == NULL ? NULL
                                                      : sbuf_string(selected_metadata_suffix));
                (void)edit_menu_append_multiline_preview(env, eb, display, syntax_highlight_item,
                                                         false, selected_preview_limit, NULL,
                                                         expanded_metadata);
                (void)sbuf_append(eb->extra, "\n");
                continue;
            }

            if (is_selected) {
                (void)sbuf_append(eb->extra, "[ic-menu-selected]");
            }
            const char* arrow = (tty_is_utf8(env->tty) ? "\xE2\x86\x92" : ">");
            (void)sbuf_appendf(eb->extra, "[!pre]%s ", (is_selected ? arrow : " "));
            if (syntax_highlight_item) {
                (void)sbuf_append(eb->extra, "[/pre]");
            }

            bool highlight_match =
                (is_filtered && !showing_all_due_to_no_matches &&
                 matches[match_idx].match_len > 0 && matches[match_idx].match_pos >= 0);
            edit_menu_append_highlighted_prefix(eb->extra, display, preview.visible_len,
                                                preview.entry_len, matches[match_idx].match_pos,
                                                matches[match_idx].match_len, is_selected,
                                                highlight_match, env, syntax_highlight_item);

            if (preview.append_ellipsis) {
                (void)sbuf_append(eb->extra, "...");
            }

            if (!syntax_highlight_item) {
                (void)sbuf_append(eb->extra, "[/pre]");
            }

            if (metadata_suffix[0] != '\0') {
                edit_menu_append_tag_text_n(eb->extra, is_selected, metadata_suffix,
                                            metadata_visible_len);
                if (metadata_truncated) {
                    (void)sbuf_append(eb->extra, "...");
                }
            }

            if (is_selected) {
                (void)sbuf_append(eb->extra, "[/ic-menu-selected]");
            }

            (void)sbuf_append(eb->extra, "\n");
        }

        edit_menu_append_scrollbar(env, eb, &menu_session.scrollbar, items_start, &window);
        edit_menu_append_scroll_hint(eb->extra, match_count, display_count, scroll_offset);
        sbuf_free(metadata_suffix_buffer);
        sbuf_free(selected_metadata_suffix);
    } else {
        scroll_offset = 0;
        if (metadata_filter_applied) {
            (void)sbuf_appendf(eb->extra,
                               "[ic-info]No history entries matched metadata filters - case %s - "
                               "scope %s - nested %s - sort %s%s[/]\n",
                               session_case_sensitive ? "sensitive" : "insensitive", scope_label,
                               nested_label, sort_label, mouse_suffix);
        } else {
            (void)sbuf_appendf(
                eb->extra,
                "[ic-info]No matches found - case %s - scope %s - nested %s - sort %s%s[/]\n",
                session_case_sensitive ? "sensitive" : "insensitive", scope_label, nested_label,
                sort_label, mouse_suffix);
        }
        if (has_live_input && selected_idx < 0) {
            history_search_append_empty_selection(env, eb);
        }
    }

    if (!env->no_help) {
        (void)sbuf_append(eb->extra, k_history_menu_footer);
    }

    edit_refresh(env, eb);

    code_t c;
    if (!edit_menu_read_event(env, eb, &menu_session, &c, &scroll_offset, &selected_idx)) {
        goto again;
    }
    code_t key_no_mods = KEY_NO_MODS(c);
    if (menu_session.mouse_scroll_enabled && key_no_mods == KEY_EVENT_MOUSE_OTHER) {
        bool accept_selection = false;
        if (edit_menu_mouse_select_vertical(env, eb, match_count, scroll_offset, last_display_count,
                                            last_status_rows, &selected_idx, &accept_selection)) {
            if (accept_selection) {
                c = KEY_TAB;
                key_no_mods = KEY_TAB;
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
        history_snapshot_free(env->history, &snap);
        mem_free(env->mem, matches);
        mem_free(env->mem, session_sort_key);
        (void)history_enable_directory(env->history, original_directory);
        (void)history_enable_directory_subdirs(env->history, original_subdirs);
        edit_menu_finish(env, eb, &menu_session, true, true);
        eb->modified = original_modified;
        return;
    } else if (c == KEY_ENTER || c == KEY_TAB) {
        const bool restore_live_input = (has_live_input && selected_idx < 0);
        if (match_count > 0 && selected_idx >= 0 && selected_idx < match_count) {
            const history_entry_t* selected =
                history_snapshot_get(&snap, matches[selected_idx].hidx);
            if (selected != NULL && selected->command != NULL) {
                editor_undo_forget(eb);
                sbuf_replace(eb->input, selected->command);
                eb->pos = sbuf_len(eb->input);
                bool expanded = edit_expand_abbreviation_if_needed(env, eb, false);
                eb->modified = expanded;
                eb->history_idx = matches[selected_idx].hidx;
            }
        }
        history_snapshot_free(env->history, &snap);
        mem_free(env->mem, matches);
        mem_free(env->mem, session_sort_key);
        if (history_directory_is_enabled(env->history) != original_directory ||
            history_directory_subdirs_is_enabled(env->history) != original_subdirs) {
            eb->history_idx = 0;
        }
        (void)history_enable_directory(env->history, original_directory);
        (void)history_enable_directory_subdirs(env->history, original_subdirs);
        edit_menu_finish(env, eb, &menu_session, restore_live_input, true);
        if (restore_live_input) {
            eb->modified = original_modified;
        }
        if (c == KEY_ENTER) {
            eb->request_submit = true;
        }
        return;
    }

    if ((KEY_MODS(c) & KEY_MOD_ALT) &&
        (key_no_mods == 'd' || key_no_mods == 'D' || key_no_mods == 'n' || key_no_mods == 'N')) {
        if (key_no_mods == 'd' || key_no_mods == 'D') {
            (void)history_enable_directory(env->history,
                                           !history_directory_is_enabled(env->history));
        } else {
            (void)history_enable_directory_subdirs(
                env->history, !history_directory_subdirs_is_enabled(env->history));
        }
        selected_idx = (has_live_input ? -1 : 0);
        scroll_offset = 0;
        matches_dirty = true;
        goto again;
    }
    if ((KEY_MODS(c) & KEY_MOD_ALT) && (key_no_mods == 's' || key_no_mods == 'S')) {
        if (history_search_cycle_sort(env, &snap, matches, match_count, &session_sort,
                                      &session_sort_key)) {
            selected_idx = (has_live_input ? -1 : 0);
            scroll_offset = 0;
            sort_dirty = true;
        } else {
            term_beep(env->term);
        }
        goto again;
    }
    const edit_menu_input_t change = edit_menu_handle_input(
        env, eb, c, &menu_session, match_count, last_display_count, last_max_scroll, &scroll_offset,
        &selected_idx, &session_case_sensitive, has_live_input);
    if (change == EDIT_MENU_INPUT_QUERY) {
        selected_idx = has_live_input ? -1 : 0;
    }
    if (change == EDIT_MENU_INPUT_QUERY || change == EDIT_MENU_INPUT_CASE) {
        matches_dirty = true;
    } else if (change == EDIT_MENU_INPUT_UNHANDLED) {
        term_beep(env->term);
    }
    goto again;
}

static void edit_history_search_with_current_line(ic_env_t* env, editor_t* eb) {
    char* initial = NULL;
    ssize_t line_start = 0;
    ssize_t line_end = 0;
    if (edit_get_line_bounds(eb, &line_start, &line_end) && line_end > line_start) {
        initial = mem_strndup(eb->mem, sbuf_string(eb->input) + line_start, line_end - line_start);
    }
    edit_history_fuzzy_search(env, eb, initial);
    mem_free(env->mem, initial);
}
