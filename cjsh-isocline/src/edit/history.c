/*
  history.c

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

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <stdint.h>
#include <sys/types.h>
#include "isocline.h"

#include "history.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>  // IWYU pragma: keep
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "common.h"
#include "fuzzy_match.h"
#include "stringbuf.h"

#define IC_DEFAULT_HISTORY (200)
#define IC_ABSOLUTE_MAX_HISTORY (5000)

struct history_s {
    const char* fname;
    alloc_t* mem;
    bool allow_duplicates;
    bool auto_add;
    bool fuzzy_case_sensitive;
    bool directory_aware;
    bool directory_subdirs;
    bool directory_parents;
    char* directory;
    size_t directory_revision;
    ssize_t max_entries;
    char* scratch;
    ssize_t scratch_cap;
    char* pending;  // This session's unfinished readline entry; never persisted.
    bool persistence_failed;
};

typedef struct history_list_s {
    history_entry_t* entries;
    ssize_t count;
    ssize_t capacity;
} history_list_t;

typedef struct history_query_filter_s {
    char* key;
    char* value;
} history_query_filter_t;

static const char* k_history_timestamp_key = "timestamp";
static const char* k_history_frequency_key = "frequency";

static bool history_metadata_key_valid(const char* key) {
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

static void history_entry_clear(history_t* h, history_entry_t* entry) {
    if (h == NULL || entry == NULL) {
        return;
    }
    if (entry->command != NULL) {
        mem_free(h->mem, entry->command);
        entry->command = NULL;
    }
    if (entry->metadata != NULL) {
        for (ssize_t i = 0; i < entry->metadata_count; ++i) {
            mem_free(h->mem, entry->metadata[i].key);
            mem_free(h->mem, entry->metadata[i].value);
        }
        mem_free(h->mem, entry->metadata);
        entry->metadata = NULL;
    }
    entry->metadata_count = 0;
    entry->metadata_capacity = 0;
}

static bool history_entry_reserve_metadata(history_t* h, history_entry_t* entry, ssize_t needed) {
    if (entry == NULL || h == NULL) {
        return false;
    }
    if (needed <= entry->metadata_capacity) {
        return true;
    }
    ssize_t new_capacity = (entry->metadata_capacity == 0 ? 4 : entry->metadata_capacity);
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    history_metadata_t* resized =
        mem_realloc_tp(h->mem, history_metadata_t, entry->metadata, new_capacity);
    if (resized == NULL) {
        return false;
    }
    entry->metadata = resized;
    entry->metadata_capacity = new_capacity;
    return true;
}

static const char* history_entry_metadata_lookup(const history_entry_t* entry, const char* key,
                                                 ssize_t* idx_out) {
    if (entry == NULL || key == NULL) {
        return NULL;
    }
    for (ssize_t i = 0; i < entry->metadata_count; ++i) {
        const char* existing_key = entry->metadata[i].key;
        if (existing_key != NULL && ic_stricmp(existing_key, key) == 0) {
            if (idx_out != NULL) {
                *idx_out = i;
            }
            return entry->metadata[i].value;
        }
    }
    return NULL;
}

static bool history_entry_set_metadata(history_t* h, history_entry_t* entry, const char* key,
                                       const char* value);

static bool history_metadata_read_frequency(const char* value, long long* frequency_out) {
    if (frequency_out != NULL) {
        *frequency_out = 0;
    }
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long long parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 1) {
        return false;
    }

    if (frequency_out != NULL) {
        *frequency_out = parsed;
    }
    return true;
}

static long long history_entry_frequency(const history_entry_t* entry) {
    long long frequency = 0;
    if (history_metadata_read_frequency(
            history_entry_metadata_lookup(entry, k_history_frequency_key, NULL), &frequency)) {
        return frequency;
    }
    return 1;
}

static bool history_entry_set_frequency(history_t* h, history_entry_t* entry, long long frequency) {
    if (h == NULL || entry == NULL) {
        return false;
    }
    if (frequency < 1) {
        frequency = 1;
    }

    char freq_buf[32];
    int n = snprintf(freq_buf, sizeof(freq_buf), "%lld", frequency);
    if (n <= 0 || n >= (int)sizeof(freq_buf)) {
        return false;
    }

    return history_entry_set_metadata(h, entry, k_history_frequency_key, freq_buf);
}

static bool history_entry_set_metadata_owned(history_t* h, history_entry_t* entry, char* key,
                                             char* value) {
    if (h == NULL || entry == NULL || key == NULL) {
        return false;
    }
    if (!history_metadata_key_valid(key)) {
        mem_free(h->mem, key);
        mem_free(h->mem, value);
        return false;
    }
    if (value == NULL) {
        value = mem_strdup(h->mem, "");
        if (value == NULL) {
            mem_free(h->mem, key);
            return false;
        }
    }

    ssize_t existing_idx = -1;
    (void)history_entry_metadata_lookup(entry, key, &existing_idx);
    if (existing_idx >= 0 && existing_idx < entry->metadata_count) {
        mem_free(h->mem, entry->metadata[existing_idx].key);
        mem_free(h->mem, entry->metadata[existing_idx].value);
        entry->metadata[existing_idx].key = key;
        entry->metadata[existing_idx].value = value;
        return true;
    }

    if (!history_entry_reserve_metadata(h, entry, entry->metadata_count + 1)) {
        mem_free(h->mem, key);
        mem_free(h->mem, value);
        return false;
    }

    entry->metadata[entry->metadata_count].key = key;
    entry->metadata[entry->metadata_count].value = value;
    entry->metadata_count++;
    return true;
}

static bool history_entry_set_metadata(history_t* h, history_entry_t* entry, const char* key,
                                       const char* value) {
    if (h == NULL || entry == NULL || key == NULL) {
        return false;
    }
    char* key_copy = mem_strdup(h->mem, key);
    char* value_copy = mem_strdup(h->mem, value == NULL ? "" : value);
    if (key_copy == NULL || value_copy == NULL) {
        mem_free(h->mem, key_copy);
        mem_free(h->mem, value_copy);
        return false;
    }
    return history_entry_set_metadata_owned(h, entry, key_copy, value_copy);
}

static bool history_entry_set_current_timestamp(history_t* h, history_entry_t* entry) {
    if (h == NULL || entry == NULL) {
        return false;
    }

    char ts_buf[32];
    int n = snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)time(NULL));
    if (n <= 0 || n >= (int)sizeof(ts_buf)) {
        return false;
    }
    return history_entry_set_metadata(h, entry, k_history_timestamp_key, ts_buf);
}

ic_private const char* history_entry_get_metadata(const history_entry_t* entry, const char* key) {
    return history_entry_metadata_lookup(entry, key, NULL);
}

static bool history_is_disabled(const history_t* h) {
    return (h == NULL || h->max_entries == 0 || h->persistence_failed);
}

static void history_list_init(history_list_t* list) {
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void history_list_free(history_t* h, history_list_t* list) {
    if (list->entries == NULL) {
        return;
    }
    for (ssize_t i = 0; i < list->count; i++) {
        history_entry_clear(h, &list->entries[i]);
    }
    mem_free(h->mem, list->entries);
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool history_list_reserve(history_t* h, history_list_t* list, ssize_t needed) {
    if (needed <= list->capacity) {
        return true;
    }
    ssize_t new_capacity = (list->capacity == 0) ? 16 : list->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    history_entry_t* new_entries =
        mem_realloc_tp(h->mem, history_entry_t, list->entries, new_capacity);
    if (new_entries == NULL) {
        return false;
    }
    list->entries = new_entries;
    list->capacity = new_capacity;
    return true;
}

static bool history_list_append(history_t* h, history_list_t* list, history_entry_t entry) {
    if (entry.command == NULL) {
        return true;
    }
    if (!history_list_reserve(h, list, list->count + 1)) {
        history_entry_clear(h, &entry);
        return false;
    }
    list->entries[list->count] = entry;
    list->count++;
    return true;
}

static void history_list_remove_at(history_t* h, history_list_t* list, ssize_t idx) {
    if (idx < 0 || idx >= list->count) {
        return;
    }
    history_entry_clear(h, &list->entries[idx]);
    if (idx < list->count - 1) {
        memmove(&list->entries[idx], &list->entries[idx + 1],
                (size_t)(list->count - idx - 1) * sizeof(history_entry_t));
    }
    list->count--;
    list->entries[list->count].command = NULL;
    list->entries[list->count].metadata = NULL;
    list->entries[list->count].metadata_count = 0;
    list->entries[list->count].metadata_capacity = 0;
}

static bool history_write_successful(int result) {
    if (result < 0) {
        debug_msg("history: stream write failed\n");
        return false;
    }
    return true;
}

static bool history_close_stream(FILE* f) {
    if (f == NULL) {
        return true;
    }
    if (fclose(f) != 0) {
        debug_msg("history: fclose failed\n");
        return false;
    }
    return true;
}

static void history_list_prune_to_max(history_t* h, history_list_t* list) {
    if (h->max_entries < 0) {
        return;
    }
    if (h->max_entries == 0) {
        while (list->count > 0) {
            history_list_remove_at(h, list, 0);
        }
        return;
    }
    while (list->count > h->max_entries) {
        history_list_remove_at(h, list, 0);
    }
}

static const char* history_entry_directory(const history_entry_t* entry) {
    const char* directory = history_entry_get_metadata(entry, "cwd");
    return directory == NULL ? "" : directory;
}

static int history_compare_entry_identity(const history_entry_t* lhs, const history_entry_t* rhs) {
    const int order = strcmp(lhs->command, rhs->command);
    return order != 0 ? order : strcmp(history_entry_directory(lhs), history_entry_directory(rhs));
}

static int compare_history_entries_by_command(const void* left, const void* right) {
    const history_entry_t* lhs = *(history_entry_t* const*)left;
    const history_entry_t* rhs = *(history_entry_t* const*)right;
    const int order = history_compare_entry_identity(lhs, rhs);
    if (order != 0) {
        return order;
    }

    // The pointers refer to entries in the same array. Keep the newest entry
    // first within each group of equal commands and directories, with all metadata.
    return (lhs > rhs) ? -1 : (lhs < rhs) ? 1 : 0;
}

static bool history_list_remove_duplicates(history_t* h, history_list_t* list) {
    if (h == NULL || list == NULL || h->allow_duplicates || list->count < 2) {
        return true;
    }

    history_entry_t** sorted = mem_malloc_tp_n(h->mem, history_entry_t*, list->count);
    if (sorted == NULL) {
        return false;
    }
    ssize_t sorted_count = 0;
    for (ssize_t i = 0; i < list->count; i++) {
        if (list->entries[i].command != NULL) {
            sorted[sorted_count++] = &list->entries[i];
        }
    }
    qsort(sorted, (size_t)sorted_count, sizeof(*sorted), compare_history_entries_by_command);

    history_entry_t* newest = NULL;
    for (ssize_t i = 0; i < sorted_count; i++) {
        if (newest != NULL && history_compare_entry_identity(newest, sorted[i]) == 0) {
            history_entry_clear(h, sorted[i]);
        } else {
            newest = sorted[i];
        }
    }
    mem_free(h->mem, sorted);

    // Compact once in original order instead of shifting the tail per duplicate.
    ssize_t kept = 0;
    for (ssize_t i = 0; i < list->count; i++) {
        if (list->entries[i].command != NULL) {
            if (kept != i) {
                list->entries[kept] = list->entries[i];
                list->entries[i] = (history_entry_t){0};
            }
            kept++;
        }
    }
    list->count = kept;
    return true;
}

static void history_persistence_error(history_t* h);
static bool history_collect_entries(history_t* h, history_list_t* list, bool dedup);
static bool history_write_all(const history_t* h, const history_list_t* list);
static bool history_collect_disk_entries(history_t* h, history_list_t* list, bool dedup);

static bool history_list_remove_value(history_t* h, history_list_t* list,
                                      const history_entry_t* value) {
    if (list == NULL || value == NULL) {
        return false;
    }
    bool removed = false;
    for (ssize_t i = list->count - 1; i >= 0; i--) {
        if (list->entries[i].command != NULL &&
            history_compare_entry_identity(&list->entries[i], value) == 0) {
            history_list_remove_at(h, list, i);
            removed = true;
        }
    }
    return removed;
}

static const history_entry_t* history_list_find_last_value(const history_list_t* list,
                                                           const history_entry_t* value) {
    if (list == NULL || value == NULL) {
        return NULL;
    }
    for (ssize_t i = list->count - 1; i >= 0; --i) {
        if (list->entries[i].command != NULL &&
            history_compare_entry_identity(&list->entries[i], value) == 0) {
            return &list->entries[i];
        }
    }
    return NULL;
}

ic_private bool history_snapshot_load(history_t* h, history_snapshot_t* snap, bool dedup) {
    if (snap == NULL) {
        return false;
    }
    if (h == NULL) {
        return false;
    }
    history_snapshot_free(h, snap);
    snap->dedup = dedup;
    snap->disabled = history_is_disabled(h);
    snap->max_entries = h->max_entries;
    snap->allow_duplicates = h->allow_duplicates;
    snap->directory_revision = h->directory_revision;
    snap->had_pending = !history_is_disabled(h) && h->pending != NULL;
    snap->has_file_status = h->fname != NULL && stat(h->fname, &snap->file_status) == 0;
    if (history_is_disabled(h)) {
        snap->loaded = true;
        snap->entries = NULL;
        snap->count = 0;
        snap->capacity = 0;
        return true;
    }

    history_list_t list;
    history_list_init(&list);
    if (!history_collect_entries(h, &list, dedup)) {
        history_list_free(h, &list);
        return false;
    }

    snap->entries = list.entries;
    snap->count = list.count;
    snap->capacity = list.capacity;
    snap->loaded = true;
    return true;
}

ic_private bool history_snapshot_is_current(const history_t* h, const history_snapshot_t* snap) {
    if (h == NULL || snap == NULL || !snap->loaded || snap->max_entries != h->max_entries ||
        snap->disabled != history_is_disabled(h) || snap->allow_duplicates != h->allow_duplicates ||
        snap->directory_revision != h->directory_revision) {
        return false;
    }
    const bool has_pending = !history_is_disabled(h) && h->pending != NULL;
    if (has_pending != snap->had_pending) {
        return false;
    }
    if (has_pending) {
        const history_entry_t* newest = history_snapshot_get(snap, 0);
        if (newest == NULL || newest->command == NULL || strcmp(newest->command, h->pending) != 0) {
            return false;
        }
    }
    struct stat current;
    const bool exists = h->fname != NULL && stat(h->fname, &current) == 0;
    if (!exists) {
        return !snap->has_file_status && (h->fname == NULL || errno == ENOENT);
    }
    const struct stat* old = &snap->file_status;
    if (!snap->has_file_status || current.st_dev != old->st_dev || current.st_ino != old->st_ino ||
        current.st_size != old->st_size || current.st_mtime != old->st_mtime ||
        current.st_ctime != old->st_ctime) {
        return false;
    }
#if defined(__APPLE__)
    return current.st_mtimespec.tv_nsec == old->st_mtimespec.tv_nsec &&
           current.st_ctimespec.tv_nsec == old->st_ctimespec.tv_nsec;
#elif !defined(_WIN32)
    return current.st_mtim.tv_nsec == old->st_mtim.tv_nsec &&
           current.st_ctim.tv_nsec == old->st_ctim.tv_nsec;
#else
    return true;
#endif
}

ic_private bool history_snapshot_refresh(history_t* h, history_snapshot_t* snap, bool dedup) {
    if (snap != NULL && snap->dedup == dedup && history_snapshot_is_current(h, snap)) {
        return true;
    }
    return history_snapshot_load(h, snap, dedup);
}

ic_private void history_snapshot_free(history_t* h, history_snapshot_t* snap) {
    if (snap == NULL) {
        return;
    }
    if (h != NULL && snap->entries != NULL) {
        for (ssize_t i = 0; i < snap->count; ++i) {
            history_entry_clear(h, &snap->entries[i]);
        }
        mem_free(h->mem, snap->entries);
    }
    *snap = (history_snapshot_t){0};
}

ic_private const history_entry_t* history_snapshot_get(const history_snapshot_t* snap, ssize_t n) {
    if (snap == NULL || snap->entries == NULL) {
        return NULL;
    }
    if (n < 0 || n >= snap->count) {
        return NULL;
    }
    ssize_t idx = snap->count - n - 1;
    if (idx < 0 || idx >= snap->count) {
        return NULL;
    }
    return &snap->entries[idx];
}

ic_private ssize_t history_snapshot_count(const history_snapshot_t* snap) {
    if (snap == NULL) {
        return 0;
    }
    return snap->count;
}

static bool history_char_is_blank(char c) {
    return (c == ' ' || c == '\t');
}

static ssize_t history_find_first_content(const char* entry, ssize_t len) {
    bool line_has_content = false;
    ssize_t line_start = 0;

    for (ssize_t i = 0; i < len; i++) {
        char c = entry[i];
        if (c == '\n' || c == '\r') {
            if (line_has_content) {
                return line_start;
            }
            line_start = i + 1;
            line_has_content = false;
        } else if (!history_char_is_blank(c)) {
            line_has_content = true;
        }
    }

    if (line_has_content) {
        return line_start;
    }

    return len;
}

static ssize_t history_find_last_content(const char* entry, ssize_t len) {
    ssize_t last_non_empty_end = 0;
    bool line_has_content = false;

    for (ssize_t i = 0; i < len; i++) {
        char c = entry[i];
        if (c == '\n' || c == '\r') {
            if (line_has_content) {
                last_non_empty_end = i;
            }
            line_has_content = false;
        } else if (!history_char_is_blank(c)) {
            line_has_content = true;
        }
    }

    if (line_has_content) {
        last_non_empty_end = len;
    }

    return last_non_empty_end;
}

static char* history_entry_dup_trimmed(alloc_t* mem, const char* entry) {
    if (entry == NULL) {
        return NULL;
    }

    ssize_t len = ic_strlen(entry);
    ssize_t start = history_find_first_content(entry, len);

    if (start >= len) {
        return mem_strdup(mem, "");
    }

    ssize_t end = history_find_last_content(entry, len);

    if (start == 0 && end == len) {
        return mem_strdup(mem, entry);
    }

    return mem_strndup(mem, entry + start, end - start);
}

static bool history_entry_normalize_metadata(history_t* h, history_entry_t* entry,
                                             long long default_frequency) {
    if (h == NULL || entry == NULL) {
        return false;
    }

    const bool use_defaults = (entry->metadata_count == 0);
    const char* timestamp = history_entry_metadata_lookup(entry, k_history_timestamp_key, NULL);
    const char* frequency = history_entry_metadata_lookup(entry, k_history_frequency_key, NULL);

    if ((use_defaults || (timestamp != NULL && strcmp(timestamp, "0") == 0)) &&
        !history_entry_set_current_timestamp(h, entry)) {
        return false;
    }
    if ((use_defaults || (frequency != NULL && strcmp(frequency, "0") == 0)) &&
        !history_entry_set_frequency(h, entry, default_frequency)) {
        return false;
    }
    return true;
}

ic_private history_t* history_new(alloc_t* mem) {
    history_t* h = mem_zalloc_tp(mem, history_t);
    if (h == NULL) {
        return NULL;
    }
    h->mem = mem;
    h->allow_duplicates = false;
    h->auto_add = true;
    h->fuzzy_case_sensitive = true;
    h->max_entries = IC_DEFAULT_HISTORY;
    h->scratch = NULL;
    h->scratch_cap = 0;
    return h;
}

ic_private void history_free(history_t* h) {
    if (h == NULL) {
        return;
    }
    if (h->scratch != NULL) {
        mem_free(h->mem, h->scratch);
        h->scratch = NULL;
        h->scratch_cap = 0;
    }
    mem_free(h->mem, h->pending);
    mem_free(h->mem, h->directory);
    mem_free(h->mem, h->fname);
    h->fname = NULL;
    mem_free(h->mem, h);
}

ic_private bool history_enable_auto_add(history_t* h, bool enable) {
    if (h == NULL) {
        return false;
    }
    bool previous = h->auto_add;
    h->auto_add = enable;
    return previous;
}

ic_private bool history_enable_duplicates(history_t* h, bool enable) {
    bool prev = h->allow_duplicates;
    h->allow_duplicates = enable;
    return prev;
}

ic_private bool history_set_fuzzy_case_sensitive(history_t* h, bool enable) {
    if (h == NULL) {
        return true;
    }
    bool prev = h->fuzzy_case_sensitive;
    h->fuzzy_case_sensitive = enable;
    return prev;
}

ic_private bool history_is_fuzzy_case_sensitive(const history_t* h) {
    if (h == NULL) {
        return true;
    }
    return h->fuzzy_case_sensitive;
}

ic_private bool history_enable_directory(history_t* h, bool enable) {
    if (h == NULL) {
        return false;
    }
    bool previous = h->directory_aware;
    if (previous != enable) {
        h->directory_aware = enable;
        h->directory_revision++;
    }
    return previous;
}

ic_private bool history_directory_is_enabled(const history_t* h) {
    return h != NULL && h->directory_aware;
}

ic_private bool history_enable_directory_subdirs(history_t* h, bool enable) {
    if (h == NULL) {
        return false;
    }
    bool previous = h->directory_subdirs;
    if (previous != enable) {
        h->directory_subdirs = enable;
        h->directory_revision++;
    }
    return previous;
}

ic_private bool history_directory_subdirs_is_enabled(const history_t* h) {
    return h != NULL && h->directory_subdirs;
}

ic_private bool history_enable_directory_parents(history_t* h, bool enable) {
    if (h == NULL) {
        return false;
    }
    bool previous = h->directory_parents;
    if (previous != enable) {
        h->directory_parents = enable;
        h->directory_revision++;
    }
    return previous;
}

ic_private bool history_directory_parents_is_enabled(const history_t* h) {
    return h != NULL && h->directory_parents;
}

ic_private bool history_set_directory(history_t* h, const char* directory) {
    if (h == NULL) {
        return false;
    }
    if (directory == NULL) {
        directory = "";
    }
    if (h->directory != NULL && strcmp(h->directory, directory) == 0) {
        return true;
    }
    char* copy = mem_strdup(h->mem, directory);
    mem_free(h->mem, h->directory);
    h->directory = copy;
    h->directory_revision++;
    return copy != NULL;
}

static const char* history_set_scratch(history_t* h, const char* entry) {
    if (entry == NULL) {
        return NULL;
    }
    ssize_t needed = ic_strlen(entry) + 1;
    if (needed > h->scratch_cap) {
        char* newscratch = mem_realloc_tp(h->mem, char, h->scratch, needed);
        if (newscratch == NULL) {
            return NULL;
        }
        h->scratch = newscratch;
        h->scratch_cap = needed;
    }
    (void)ic_strncpy(h->scratch, h->scratch_cap, entry, needed - 1);
    return h->scratch;
}

ic_private ssize_t history_count(const history_t* h) {
    if (history_is_disabled(h)) {
        return 0;
    }
    history_list_t list;
    history_list_init(&list);
    history_t* mutable_h = (history_t*)h;
    if (!history_collect_entries(mutable_h, &list, true)) {
        history_list_free(mutable_h, &list);
        return 0;
    }
    ssize_t count = list.count;
    history_list_free(mutable_h, &list);
    return count;
}

ic_private const char* history_get(const history_t* h, ssize_t n) {
    if (history_is_disabled(h)) {
        return NULL;
    }
    history_list_t list;
    history_list_init(&list);
    history_t* mutable_h = (history_t*)h;
    if (!history_collect_entries(mutable_h, &list, true)) {
        history_list_free(mutable_h, &list);
        return NULL;
    }
    const char* result = NULL;
    if (n >= 0 && n < list.count) {
        ssize_t idx = list.count - n - 1;
        result = history_set_scratch(mutable_h, list.entries[idx].command);
    }
    history_list_free(mutable_h, &list);
    return result;
}

static bool history_update_file(history_t* h, history_list_t* list) {
    if (!history_write_all(h, list)) {
        return false;
    }
    return true;
}

static bool history_push_with_metadata_unlocked(history_t* h, const char* entry,
                                                const ic_history_metadata_t* metadata,
                                                size_t metadata_count);

static bool history_update_unlocked(history_t* h, const char* entry) {
    if (h == NULL || entry == NULL || history_is_disabled(h)) {
        return false;
    }

    history_list_t list;
    history_list_init(&list);
    if (!history_collect_disk_entries(h, &list, false)) {
        history_list_free(h, &list);
        return false;
    }

    if (list.count == 0) {
        history_list_free(h, &list);
        return history_push_with_metadata_unlocked(h, entry, NULL, 0);
    }

    char* normalized = history_entry_dup_trimmed(h->mem, entry);
    if (normalized == NULL) {
        history_list_free(h, &list);
        return false;
    }

    history_entry_t* last = &list.entries[list.count - 1];
    mem_free(h->mem, last->command);
    last->command = normalized;

    history_list_prune_to_max(h, &list);

    bool ok = history_update_file(h, &list);
    history_list_free(h, &list);
    return ok;
}

ic_private bool history_push(history_t* h, const char* entry) {
    return history_push_with_metadata(h, entry, NULL, 0);
}

static bool history_push_with_metadata_unlocked(history_t* h, const char* entry,
                                                const ic_history_metadata_t* metadata,
                                                size_t metadata_count) {
    if (h == NULL || entry == NULL || history_is_disabled(h)) {
        return false;
    }

    char* normalized = history_entry_dup_trimmed(h->mem, entry);
    if (normalized == NULL) {
        return false;
    }

    history_list_t list;
    history_list_init(&list);
    if (!history_collect_disk_entries(h, &list, false)) {
        history_list_free(h, &list);
        mem_free(h->mem, normalized);
        return false;
    }

    history_entry_t new_entry = {
        .command = normalized,
        .metadata = NULL,
        .metadata_count = 0,
        .metadata_capacity = 0,
    };

    for (size_t i = 0; i < metadata_count; ++i) {
        if (metadata == NULL || metadata[i].key == NULL) {
            continue;
        }
        if (!history_entry_set_metadata(h, &new_entry, metadata[i].key,
                                        metadata[i].value == NULL ? "" : metadata[i].value)) {
            history_entry_clear(h, &new_entry);
            history_list_free(h, &list);
            return false;
        }
    }
    long long next_frequency = 1;
    if (!h->allow_duplicates) {
        const history_entry_t* last_entry = history_list_find_last_value(&list, &new_entry);
        if (last_entry != NULL) {
            long long previous_frequency = history_entry_frequency(last_entry);
            next_frequency = (previous_frequency >= LLONG_MAX ? LLONG_MAX : previous_frequency + 1);
        }
        (void)history_list_remove_value(h, &list, &new_entry);
    }
    if (!history_entry_normalize_metadata(h, &new_entry, next_frequency)) {
        history_entry_clear(h, &new_entry);
        history_list_free(h, &list);
        return false;
    }

    if (!history_list_append(h, &list, new_entry)) {
        history_list_free(h, &list);
        return false;
    }

    history_list_prune_to_max(h, &list);
    bool ok = history_update_file(h, &list);

    history_list_free(h, &list);
    return ok;
}

static void history_remove_last_unlocked(history_t* h) {
    if (history_is_disabled(h)) {
        return;
    }
    history_list_t list;
    history_list_init(&list);
    const bool previous_allow_duplicates = h->allow_duplicates;
    h->allow_duplicates = true;
    // Keep earlier duplicates intact while removing the transient current-line entry;
    // collapsing first would discard the accumulated frequency of older commands.
    const bool collected = history_collect_disk_entries(h, &list, false);
    h->allow_duplicates = previous_allow_duplicates;
    if (!collected) {
        history_list_free(h, &list);
        return;
    }
    if (list.count > 0) {
        history_list_remove_at(h, &list, list.count - 1);
        (void)history_update_file(h, &list);
    }
    history_list_free(h, &list);
}

static void history_clear_unlocked(history_t* h) {
    if (h == NULL) {
        return;
    }
    if (h->scratch != NULL) {
        mem_free(h->mem, h->scratch);
        h->scratch = NULL;
        h->scratch_cap = 0;
    }
    if (h->fname == NULL || history_is_disabled(h)) {
        return;
    }
    history_list_t list;
    history_list_init(&list);
    (void)history_write_all(h, &list);
}

ic_private bool history_search(const history_t* h, ssize_t from, const char* search, bool backward,
                               ssize_t* hidx, ssize_t* hpos) {
    if (h == NULL || search == NULL || history_is_disabled(h)) {
        return false;
    }
    history_list_t list;
    history_list_init(&list);
    history_t* mutable_h = (history_t*)h;
    if (!history_collect_entries(mutable_h, &list, true)) {
        history_list_free(mutable_h, &list);
        return false;
    }

    const char* p = NULL;
    ssize_t found = -1;

    if (backward) {
        for (ssize_t i = from; i < list.count; i++) {
            ssize_t idx = list.count - i - 1;
            const char* cmd = list.entries[idx].command;
            if (cmd == NULL) {
                continue;
            }
            p = strstr(cmd, search);
            if (p != NULL) {
                found = i;
                break;
            }
        }
    } else {
        for (ssize_t i = from; i >= 0; i--) {
            ssize_t idx = list.count - i - 1;
            if (idx < 0 || idx >= list.count) {
                continue;
            }
            const char* cmd = list.entries[idx].command;
            if (cmd == NULL) {
                continue;
            }
            p = strstr(cmd, search);
            if (p != NULL) {
                found = i;
                break;
            }
        }
    }

    if (found >= 0 && p != NULL) {
        if (hidx != NULL) {
            *hidx = found;
        }
        if (hpos != NULL && list.entries[list.count - found - 1].command != NULL) {
            *hpos = (ssize_t)(p - list.entries[list.count - found - 1].command);
        }
        history_list_free(mutable_h, &list);
        return true;
    }

    history_list_free(mutable_h, &list);
    return false;
}

ic_private bool history_search_prefix(const history_t* h, ssize_t from, const char* prefix,
                                      bool backward, ssize_t* hidx) {
    if (prefix == NULL || h == NULL || history_is_disabled(h)) {
        return false;
    }

    history_list_t list;
    history_list_init(&list);
    history_t* mutable_h = (history_t*)h;
    if (!history_collect_entries(mutable_h, &list, true)) {
        history_list_free(mutable_h, &list);
        return false;
    }

    const size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) {
        bool result = false;
        if (backward) {
            if (from < list.count) {
                if (hidx != NULL) {
                    *hidx = from;
                }
                result = true;
            }
        } else {
            if (from >= 0) {
                if (hidx != NULL) {
                    *hidx = from;
                }
                result = true;
            }
        }
        history_list_free(mutable_h, &list);
        return result;
    }

    if (backward) {
        for (ssize_t i = from; i < list.count; i++) {
            ssize_t idx = list.count - i - 1;
            if (idx < 0 || idx >= list.count) {
                continue;
            }
            const char* entry = list.entries[idx].command;
            if (entry != NULL && strncmp(entry, prefix, prefix_len) == 0) {
                if (hidx != NULL) {
                    *hidx = i;
                }
                history_list_free(mutable_h, &list);
                return true;
            }
        }
    } else {
        for (ssize_t i = from; i >= 0; i--) {
            ssize_t idx = list.count - i - 1;
            if (idx < 0 || idx >= list.count) {
                continue;
            }
            const char* entry = list.entries[idx].command;
            if (entry != NULL && strncmp(entry, prefix, prefix_len) == 0) {
                if (hidx != NULL) {
                    *hidx = i;
                }
                history_list_free(mutable_h, &list);
                return true;
            }
        }
    }

    history_list_free(mutable_h, &list);
    return false;
}

static int compare_matches(const void* a, const void* b) {
    const history_match_t* ma = (const history_match_t*)a;
    const history_match_t* mb = (const history_match_t*)b;

    if (mb->score != ma->score) {
        return mb->score - ma->score;
    }

    return (int)(mb->hidx - ma->hidx);
}

static void history_query_filters_free(history_t* h, history_query_filter_t* filters,
                                       size_t filter_count) {
    if (h == NULL || filters == NULL) {
        return;
    }
    for (size_t i = 0; i < filter_count; ++i) {
        mem_free(h->mem, filters[i].key);
        mem_free(h->mem, filters[i].value);
    }
    mem_free(h->mem, filters);
}

static bool history_query_filters_append(history_t* h, history_query_filter_t** filters,
                                         size_t* count, size_t* capacity, char* key, char* value) {
    if (h == NULL || filters == NULL || count == NULL || capacity == NULL || key == NULL) {
        return false;
    }
    if (!history_metadata_key_valid(key)) {
        mem_free(h->mem, key);
        mem_free(h->mem, value);
        return false;
    }
    if (value == NULL) {
        value = mem_strdup(h->mem, "");
        if (value == NULL) {
            mem_free(h->mem, key);
            return false;
        }
    }
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0 ? 4 : *capacity * 2);
        history_query_filter_t* resized =
            mem_realloc_tp(h->mem, history_query_filter_t, *filters, (ssize_t)new_capacity);
        if (resized == NULL) {
            mem_free(h->mem, key);
            mem_free(h->mem, value);
            return false;
        }
        *filters = resized;
        *capacity = new_capacity;
    }
    (*filters)[*count].key = key;
    (*filters)[*count].value = value;
    (*count)++;
    return true;
}

static bool history_parse_metadata_filter_token(history_t* h, const char* token, size_t len,
                                                history_query_filter_t** filters,
                                                size_t* filter_count, size_t* filter_capacity) {
    if (h == NULL || token == NULL || len == 0) {
        return false;
    }

    for (size_t i = 0; i + 1 < len; ++i) {
        if (token[i] == ':' && token[i + 1] == ':') {
            if (i == 0) {
                return false;
            }
            ssize_t key_len = (ssize_t)i;
            const char* value_start = token + i + 2;
            ssize_t value_len = (ssize_t)((token + len) - value_start);

            if (value_len <= 0) {
                // Recognized `key::` metadata query prefix; consume the token so it does not
                // interfere with fuzzy command matching while the user continues typing.
                return true;
            }

            char* key = mem_strndup(h->mem, token, key_len);
            char* value = mem_strndup(h->mem, value_start, value_len);
            if (key == NULL || value == NULL) {
                mem_free(h->mem, key);
                mem_free(h->mem, value);
                return false;
            }
            return history_query_filters_append(h, filters, filter_count, filter_capacity, key,
                                                value);
        }
    }

    const char* delim = NULL;
    for (size_t i = 0; i < len; ++i) {
        if (token[i] == '=') {
            delim = token + i;
            break;
        }
    }
    if (delim == NULL || delim == token || delim >= token + len - 1) {
        return false;
    }

    ssize_t key_len = (ssize_t)(delim - token);
    ssize_t value_len = (ssize_t)((token + len) - (delim + 1));
    char* key = mem_strndup(h->mem, token, key_len);
    char* value = mem_strndup(h->mem, delim + 1, value_len);
    if (key == NULL || value == NULL) {
        mem_free(h->mem, key);
        mem_free(h->mem, value);
        return false;
    }
    return history_query_filters_append(h, filters, filter_count, filter_capacity, key, value);
}

static bool history_entry_matches_filters(const history_entry_t* entry,
                                          const history_query_filter_t* filters,
                                          size_t filter_count) {
    if (filter_count == 0) {
        return true;
    }
    if (entry == NULL) {
        return false;
    }
    for (size_t i = 0; i < filter_count; ++i) {
        const char* value = history_entry_get_metadata(entry, filters[i].key);
        if (value == NULL || strcmp(value, filters[i].value) != 0) {
            return false;
        }
    }
    return true;
}

ic_private bool history_snapshot_fuzzy_search(const history_t* h, const history_snapshot_t* snap,
                                              const char* query, history_match_t* matches,
                                              ssize_t max_matches, ssize_t* match_count,
                                              bool* metadata_filter_applied, bool case_sensitive) {
    if (metadata_filter_applied) {
        *metadata_filter_applied = false;
    }

    if (h == NULL || snap == NULL || query == NULL || matches == NULL || max_matches <= 0) {
        if (match_count) {
            *match_count = 0;
        }
        return false;
    }

    if (history_is_disabled(h)) {
        if (match_count) {
            *match_count = 0;
        }
        return false;
    }

    history_t* mutable_h = (history_t*)h;
    history_query_filter_t* filters = NULL;
    size_t filter_count = 0;
    size_t filter_capacity = 0;
    char* sanitized_query = NULL;
    bool sanitized_available = false;
    size_t sanitized_len = 0;

    ssize_t original_query_len = ic_strlen(query);
    if (original_query_len < 0) {
        original_query_len = 0;
    }
    sanitized_query = mem_malloc_tp_n(mutable_h->mem, char, (size_t)original_query_len + 1);
    if (sanitized_query != NULL) {
        sanitized_query[0] = '\0';
        sanitized_available = true;
    }

    const char* cursor = query;
    while (*cursor != '\0') {
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            cursor++;
        }

        const char* token_start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }

        size_t token_len = (size_t)(cursor - token_start);
        if (token_len == 0) {
            break;
        }

        if (history_parse_metadata_filter_token(mutable_h, token_start, token_len, &filters,
                                                &filter_count, &filter_capacity)) {
            continue;
        }

        if (sanitized_available) {
            if (sanitized_len > 0) {
                sanitized_query[sanitized_len++] = ' ';
            }
            ic_memcpy(sanitized_query + sanitized_len, token_start, (ssize_t)token_len);
            sanitized_len += token_len;
            sanitized_query[sanitized_len] = '\0';
        }
    }

    if (!sanitized_available && filter_count > 0) {
        sanitized_query = mem_malloc_tp_n(mutable_h->mem, char, 1);
        if (sanitized_query != NULL) {
            sanitized_query[0] = '\0';
            sanitized_available = true;
            sanitized_len = 0;
        }
    }

    const char* effective_query = query;
    if (filter_count > 0) {
        if (sanitized_available && sanitized_query != NULL) {
            sanitized_query[sanitized_len] = '\0';
            effective_query = sanitized_query;
        } else {
            effective_query = "";
        }
    }

    if (effective_query == NULL) {
        effective_query = "";
    }

    if (filter_count > 0 && metadata_filter_applied != NULL) {
        *metadata_filter_applied = true;
    }

    ssize_t count = 0;

    if (effective_query[0] == '\0') {
        for (ssize_t offset = 0; offset < snap->count && count < max_matches; offset++) {
            ssize_t idx = snap->count - offset - 1;
            const history_entry_t* entry = &snap->entries[idx];
            if (entry->command == NULL) {
                continue;
            }
            if (!history_entry_matches_filters(entry, filters, filter_count)) {
                continue;
            }

            matches[count].hidx = offset;
            matches[count].score = (int)(100 - offset);
            matches[count].match_pos = 0;
            matches[count].match_len = 0;
            count++;
        }
    } else {
        for (ssize_t offset = 0; offset < snap->count; offset++) {
            ssize_t idx = snap->count - offset - 1;
            const history_entry_t* entry = &snap->entries[idx];
            if (entry->command == NULL) {
                continue;
            }
            if (!history_entry_matches_filters(entry, filters, filter_count)) {
                continue;
            }

            ssize_t mpos = 0;
            ssize_t mlen = 0;
            int score =
                ic_fuzzy_match_score(entry->command, effective_query, &mpos, &mlen, case_sensitive);

            if (score >= 0) {
                score += (int)(offset / 10);

                if (count < max_matches) {
                    matches[count].hidx = offset;
                    matches[count].score = score;
                    matches[count].match_pos = mpos;
                    matches[count].match_len = mlen;
                    count++;
                } else {
                    ssize_t worst_idx = 0;
                    int worst_score = matches[0].score;
                    for (ssize_t j = 1; j < max_matches; j++) {
                        if (matches[j].score < worst_score) {
                            worst_score = matches[j].score;
                            worst_idx = j;
                        }
                    }

                    if (score > worst_score) {
                        matches[worst_idx].hidx = offset;
                        matches[worst_idx].score = score;
                        matches[worst_idx].match_pos = mpos;
                        matches[worst_idx].match_len = mlen;
                    }
                }
            }
        }
    }

    if (count > 1) {
        qsort(matches, count, sizeof(history_match_t), compare_matches);
    }

    if (match_count) {
        *match_count = count;
    }

    if (sanitized_query != NULL) {
        mem_free(mutable_h->mem, sanitized_query);
    }
    history_query_filters_free(mutable_h, filters, filter_count);

    return count > 0;
}

ic_private bool history_fuzzy_search_with_case(const history_t* h, const char* query,
                                               history_match_t* matches, ssize_t max_matches,
                                               ssize_t* match_count, bool* metadata_filter_applied,
                                               bool case_sensitive) {
    history_snapshot_t snap = {0};
    const bool loaded = h != NULL && query != NULL && matches != NULL && max_matches > 0 &&
                        history_snapshot_load((history_t*)h, &snap, true);
    const bool found =
        history_snapshot_fuzzy_search(h, loaded ? &snap : NULL, query, matches, max_matches,
                                      match_count, metadata_filter_applied, case_sensitive);
    history_snapshot_free((history_t*)h, &snap);
    return found;
}

ic_private bool history_fuzzy_search(const history_t* h, const char* query,
                                     history_match_t* matches, ssize_t max_matches,
                                     ssize_t* match_count, bool* metadata_filter_applied) {
    history_t* mutable_h = (history_t*)h;
    const bool case_sensitive = history_is_fuzzy_case_sensitive(mutable_h);
    return history_fuzzy_search_with_case(h, query, matches, max_matches, match_count,
                                          metadata_filter_applied, case_sensitive);
}

ic_private void history_load_from(history_t* h, const char* fname, long max_entries) {
    if (h == NULL) {
        return;
    }

    if (h->fname != NULL) {
        mem_free(h->mem, h->fname);
        h->fname = NULL;
    }

    if (fname != NULL) {
#ifndef _WIN32
        // Follow an existing data-file symlink before choosing the lock name.
        // Sessions using aliases must share the target's lock and replacement.
        char* resolved = realpath(fname, NULL);
        h->fname = mem_strdup(h->mem, resolved == NULL ? fname : resolved);
        free(resolved);
#else
        h->fname = mem_strdup(h->mem, fname);
#endif
    }

    if (max_entries == 0) {
        h->max_entries = 0;
    } else if (max_entries < 0) {
        h->max_entries = IC_DEFAULT_HISTORY;
    } else if (max_entries > IC_ABSOLUTE_MAX_HISTORY) {
        h->max_entries = IC_ABSOLUTE_MAX_HISTORY;
    } else {
        h->max_entries = max_entries;
    }

    if (!history_is_disabled(h)) {
        history_load(h);
    }
}

static char from_xdigit(int c) {
    if (c >= '0' && c <= '9') {
        return (char)(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return (char)(10 + (c - 'A'));
    }
    if (c >= 'a' && c <= 'f') {
        return (char)(10 + (c - 'a'));
    }
    return 0;
}

static char to_xdigit(uint8_t c) {
    if (c <= 9) {
        return ((char)c + '0');
    }
    if (c >= 10 && c <= 15) {
        return ((char)c - 10 + 'A');
    }
    return '0';
}

static bool ic_isxdigit(int c) {
    return ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9'));
}

typedef enum history_decode_state_e {
    HISTORY_DECODE_TEXT,
    HISTORY_DECODE_ESCAPE,
    HISTORY_DECODE_HEX_HIGH,
    HISTORY_DECODE_HEX_LOW
} history_decode_state_t;

typedef struct history_decoder_s {
    history_decode_state_t state;
    unsigned char high;
} history_decoder_t;

// Both the streaming reader and the public buffer decoder use the same escape rules.
static bool history_decode_byte(history_decoder_t* decoder, unsigned char byte, char* value,
                                bool* emit) {
    *emit = false;
    switch (decoder->state) {
        case HISTORY_DECODE_TEXT:
            if (byte == '\\') {
                decoder->state = HISTORY_DECODE_ESCAPE;
                return true;
            }
            break;
        case HISTORY_DECODE_ESCAPE:
            decoder->state = HISTORY_DECODE_TEXT;
            switch (byte) {
                case 'n':
                    byte = '\n';
                    break;
                case 't':
                    byte = '\t';
                    break;
                case '\\':
                    break;
                case 'r':
                    return true;
                case 'x':
                    decoder->state = HISTORY_DECODE_HEX_HIGH;
                    return true;
                default:
                    return false;
            }
            break;
        case HISTORY_DECODE_HEX_HIGH:
            decoder->high = byte;
            decoder->state = HISTORY_DECODE_HEX_LOW;
            return true;
        case HISTORY_DECODE_HEX_LOW:
            if (!ic_isxdigit(decoder->high) || !ic_isxdigit(byte)) {
                return false;
            }
            byte = (unsigned char)(from_xdigit(decoder->high) * 16 + from_xdigit(byte));
            decoder->state = HISTORY_DECODE_TEXT;
            break;
    }
    *value = (char)byte;
    *emit = true;
    return true;
}

ic_public bool ic_history_decode_entry(const char* encoded, size_t encoded_length, char* decoded,
                                       size_t decoded_capacity, size_t* decoded_length) {
    if (decoded_length != NULL) {
        *decoded_length = 0;
    }
    if (encoded == NULL || decoded == NULL || decoded_capacity <= encoded_length) {
        return false;
    }
    history_decoder_t decoder = {HISTORY_DECODE_TEXT, 0};
    size_t length = 0;
    for (size_t i = 0; i < encoded_length; ++i) {
        char value = 0;
        bool emit = false;
        if (!history_decode_byte(&decoder, (unsigned char)encoded[i], &value, &emit)) {
            return false;
        }
        if (emit) {
            decoded[length++] = value;
        }
    }
    if (decoder.state != HISTORY_DECODE_TEXT) {
        return false;
    }
    decoded[length] = '\0';
    if (decoded_length != NULL) {
        *decoded_length = length;
    }
    return true;
}

static char* history_read_entry(history_t* h, FILE* f, stringbuf_t* sbuf) {
    sbuf_clear(sbuf);
    history_decoder_t decoder = {HISTORY_DECODE_TEXT, 0};
    while (true) {
        int c = fgetc(f);
        if (c == EOF) {
            if (ferror(f) || decoder.state != HISTORY_DECODE_TEXT) {
                return NULL;
            }
            break;
        }
        if (c == '\n' && decoder.state == HISTORY_DECODE_TEXT) {
            break;
        }
        char value = 0;
        bool emit = false;
        if (!history_decode_byte(&decoder, (unsigned char)c, &value, &emit)) {
            return NULL;
        }
        if (emit) {
            (void)sbuf_append_char(sbuf, value);
        }
    }
    if (sbuf_len(sbuf) == 0) {
        return mem_strdup(h->mem, "");
    }
    if (sbuf_string(sbuf)[0] == '#') {
        return NULL;
    }
    return history_entry_dup_trimmed(h->mem, sbuf_string(sbuf));
}

static bool history_write_entry(const char* entry, FILE* f, stringbuf_t* sbuf) {
    sbuf_clear(sbuf);

    if (entry == NULL) {
        return true;
    }

    if (*entry == '\0') {
        return history_write_successful(fputc('\n', f));
    }

    while (*entry != 0) {
        char c = *entry++;
        if (c == '\\') {
            (void)sbuf_append(sbuf, "\\\\");
        } else if (c == '\n') {
            (void)sbuf_append(sbuf, "\\n");
        } else if (c == '\r') {
            continue;
        } else if (c == '\t') {
            (void)sbuf_append(sbuf, "\\t");
        } else if (c < ' ' || c > '~' || c == '#') {
            char c1 = to_xdigit((uint8_t)c / 16);
            char c2 = to_xdigit((uint8_t)c % 16);
            (void)sbuf_append(sbuf, "\\x");
            (void)sbuf_append_char(sbuf, c1);
            (void)sbuf_append_char(sbuf, c2);
        } else {
            (void)sbuf_append_char(sbuf, c);
        }
    }

    if (sbuf_len(sbuf) > 0) {
        (void)sbuf_append(sbuf, "\n");
        if (!history_write_successful(fputs(sbuf_string(sbuf), f))) {
            return false;
        }
    }
    return true;
}

static bool history_metadata_write_escaped(stringbuf_t* sbuf, const char* value) {
    if (sbuf == NULL) {
        return false;
    }
    if (value == NULL) {
        return true;
    }
    for (const char* p = value; *p != '\0'; ++p) {
        uint8_t c = (uint8_t)(*p);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            (void)sbuf_append_char(sbuf, (char)c);
        } else {
            (void)sbuf_append_char(sbuf, '%');
            (void)sbuf_append_char(sbuf, to_xdigit((uint8_t)(c / 16)));
            (void)sbuf_append_char(sbuf, to_xdigit((uint8_t)(c % 16)));
        }
    }
    return true;
}

static char* history_metadata_decode_escaped(history_t* h, const char* encoded) {
    if (h == NULL || encoded == NULL) {
        return NULL;
    }
    ssize_t len = (ssize_t)strlen(encoded);
    char* out = mem_malloc_tp_n(h->mem, char, len + 1);
    if (out == NULL) {
        return NULL;
    }
    ssize_t o = 0;
    for (ssize_t i = 0; i < len; ++i) {
        char c = encoded[i];
        if (c == '%' && i + 2 < len && ic_isxdigit(encoded[i + 1]) && ic_isxdigit(encoded[i + 2])) {
            uint8_t hi = (uint8_t)from_xdigit(encoded[i + 1]);
            uint8_t lo = (uint8_t)from_xdigit(encoded[i + 2]);
            out[o++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return out;
}

static bool history_parse_metadata_header_line(history_t* h, history_entry_t* entry,
                                               const char* header_line) {
    if (h == NULL || entry == NULL || header_line == NULL) {
        return false;
    }

    const char* cursor = header_line + 1;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r') {
        return false;
    }

    bool has_equals = false;
    for (const char* p = cursor; *p != '\0'; ++p) {
        if (*p == '=') {
            has_equals = true;
            break;
        }
        if (*p == '\n' || *p == '\r') {
            break;
        }
    }

    if (!has_equals) {
        return false;
    }

    const char* token = cursor;
    while (*token != '\0' && *token != '\n' && *token != '\r') {
        while (*token == ' ' || *token == '\t') {
            token++;
        }
        if (*token == '\0' || *token == '\n' || *token == '\r') {
            break;
        }

        const char* token_end = token;
        while (*token_end != '\0' && *token_end != '\n' && *token_end != '\r' &&
               *token_end != ' ' && *token_end != '\t') {
            token_end++;
        }

        const char* equals = token;
        while (equals < token_end && *equals != '=') {
            equals++;
        }

        if (equals > token && equals < token_end) {
            char* key = mem_strndup(h->mem, token, (ssize_t)(equals - token));
            char* encoded_value =
                mem_strndup(h->mem, equals + 1, (ssize_t)(token_end - equals - 1));
            char* decoded =
                history_metadata_decode_escaped(h, encoded_value == NULL ? "" : encoded_value);
            mem_free(h->mem, encoded_value);
            if (key != NULL && decoded != NULL) {
                if (!history_entry_set_metadata_owned(h, entry, key, decoded)) {
                    mem_free(h->mem, key);
                    mem_free(h->mem, decoded);
                }
            } else {
                mem_free(h->mem, key);
                mem_free(h->mem, decoded);
            }
        }

        token = token_end;
    }
    return true;
}

static bool history_write_record(const history_entry_t* entry, FILE* f, stringbuf_t* sbuf) {
    if (entry == NULL || entry->command == NULL) {
        return true;
    }

    sbuf_clear(sbuf);
    (void)sbuf_append(sbuf, "#");

    bool wrote_metadata = false;
    for (ssize_t i = 0; i < entry->metadata_count; ++i) {
        const char* key = entry->metadata[i].key;
        if (!history_metadata_key_valid(key)) {
            continue;
        }
        wrote_metadata = true;
        (void)sbuf_append_char(sbuf, ' ');
        (void)sbuf_append(sbuf, key);
        (void)sbuf_append_char(sbuf, '=');
        (void)history_metadata_write_escaped(
            sbuf, entry->metadata[i].value == NULL ? "" : entry->metadata[i].value);
    }
    if (!wrote_metadata) {
        (void)sbuf_append_char(sbuf, ' ');
        (void)sbuf_append(sbuf, k_history_frequency_key);
        (void)sbuf_append_char(sbuf, '=');
        (void)sbuf_append(sbuf, "1");

        char ts_buf[32];
        int n = snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)time(NULL));
        if (n <= 0 || n >= (int)sizeof(ts_buf)) {
            return false;
        }
        (void)sbuf_append_char(sbuf, ' ');
        (void)sbuf_append(sbuf, k_history_timestamp_key);
        (void)sbuf_append_char(sbuf, '=');
        (void)history_metadata_write_escaped(sbuf, ts_buf);
    }
    (void)sbuf_append_char(sbuf, '\n');

    if (!history_write_successful(fputs(sbuf_string(sbuf), f))) {
        return false;
    }

    return history_write_entry(entry->command, f, sbuf);
}

static bool history_collect_disk_entries(history_t* h, history_list_t* list, bool dedup) {
    history_list_init(list);
    if (h == NULL) {
        return false;
    }
    if (history_is_disabled(h)) {
        return true;
    }
    if (h->fname == NULL) {
        return true;
    }

    struct stat st;
    if (stat(h->fname, &st) == 0 && !S_ISREG(st.st_mode)) {
        return false;
    }
    FILE* f = fopen(h->fname, "r");
    if (f == NULL) {
        return errno == ENOENT;
    }

    stringbuf_t* sbuf = sbuf_new(h->mem);
    if (sbuf == NULL) {
        (void)history_close_stream(f);
        return false;
    }

    bool success = true;
    char header_buf[512];
    while (success) {
        if (feof(f)) {
            clearerr(f);
            break;
        }

        int c = fgetc(f);
        if (c == EOF) {
            if (ferror(f)) {
                success = false;
            } else {
                clearerr(f);
            }
            break;
        }
        if (c == '\n' || c == '\r') {
            continue;
        }

        history_entry_t entry = {
            .command = NULL,
            .metadata = NULL,
            .metadata_count = 0,
            .metadata_capacity = 0,
        };

        if (c == '#') {
            if (ungetc(c, f) == EOF) {
                success = false;
                break;
            }
            errno = 0;
            if (fgets(header_buf, sizeof(header_buf), f) == NULL) {
                if (ferror(f)) {
                    success = false;
                }
                break;
            }

            if (!history_parse_metadata_header_line(h, &entry, header_buf)) {
                history_entry_clear(h, &entry);
                continue;
            }

            long pos_after_header = ftell(f);
            if (pos_after_header < 0) {
                success = false;
                break;
            }

            int next_char = fgetc(f);
            if (next_char == EOF) {
                if (ferror(f)) {
                    success = false;
                }
                break;
            }
            if (next_char == '#') {
                if (fseek(f, pos_after_header, SEEK_SET) != 0) {
                    success = false;
                    break;
                }
                clearerr(f);
                continue;
            }
            if (ungetc(next_char, f) == EOF) {
                clearerr(f);
                success = false;
                break;
            }
        } else {
            if (ungetc(c, f) == EOF) {
                clearerr(f);
                success = false;
                break;
            }
        }

        clearerr(f);

        char* command = history_read_entry(h, f, sbuf);
        if (command == NULL) {
            if (ferror(f)) {
                history_entry_clear(h, &entry);
                success = false;
                break;
            }
            if (feof(f)) {
                history_entry_clear(h, &entry);
                break;
            }
            clearerr(f);
            history_entry_clear(h, &entry);
            continue;
        }

        entry.command = command;
        if (!history_entry_normalize_metadata(h, &entry, history_entry_frequency(&entry))) {
            history_entry_clear(h, &entry);
            history_list_free(h, list);
            sbuf_free(sbuf);
            (void)history_close_stream(f);
            return false;
        }

        if (!history_list_append(h, list, entry)) {
            history_list_free(h, list);
            sbuf_free(sbuf);
            (void)history_close_stream(f);
            return false;
        }
    }

    sbuf_free(sbuf);
    bool close_ok = history_close_stream(f);

    if (!success) {
        history_list_free(h, list);
        return false;
    }

    history_list_prune_to_max(h, list);
    if ((dedup || !h->allow_duplicates) && !history_list_remove_duplicates(h, list)) {
        history_list_free(h, list);
        return false;
    }

    return close_ok;
}

ic_private bool history_matches_directory(const history_t* h, const char* cwd) {
    if (h == NULL || !h->directory_aware) {
        return true;
    }
    if (h->directory == NULL || h->directory[0] != '/' || cwd == NULL || cwd[0] != '/') {
        return false;
    }
    size_t len = strlen(h->directory);
    while (len > 1 && h->directory[len - 1] == '/') {
        len--;
    }
    size_t cwd_len = strlen(cwd);
    while (cwd_len > 1 && cwd[cwd_len - 1] == '/') {
        cwd_len--;
    }
    if (cwd_len == len) {
        return strncmp(cwd, h->directory, len) == 0;
    }
    if (cwd_len > len) {
        return h->directory_subdirs && strncmp(cwd, h->directory, len) == 0 &&
               (len == 1 || cwd[len] == '/');
    }
    return h->directory_parents && strncmp(h->directory, cwd, cwd_len) == 0 &&
           (cwd_len == 1 || h->directory[cwd_len] == '/');
}

static bool history_collect_entries(history_t* h, history_list_t* list, bool dedup) {
    if (!history_collect_disk_entries(h, list, dedup)) {
        return false;
    }
    // Filter only the read view. Persistence must retain entries from other directories.
    ssize_t kept = 0;
    for (ssize_t i = 0; i < list->count; i++) {
        if (!history_matches_directory(h, history_entry_directory(&list->entries[i]))) {
            history_entry_clear(h, &list->entries[i]);
        } else {
            if (kept != i) {
                list->entries[kept] = list->entries[i];
                list->entries[i] = (history_entry_t){0};
            }
            kept++;
        }
    }
    list->count = kept;
    if (!history_is_disabled(h) && h->pending != NULL) {
        history_entry_t entry = {0};
        entry.command = mem_strdup(h->mem, h->pending);
        if (entry.command == NULL || !history_list_append(h, list, entry)) {
            return false;
        }
    }
    return true;
}

static void history_persistence_error(history_t* h) {
    if (!h->persistence_failed) {
        (void)fputs("cjsh: history: persistence unavailable; disabling history storage: ", stderr);
        (void)fputs(h->fname == NULL ? "(unset)" : h->fname, stderr);
        (void)fputc('\n', stderr);
        h->persistence_failed = true;
    }
}

// The lock has a stable inode across replacements of the data file. Readers
// need no lock: opening the data file sees either complete committed snapshot.
static int history_lock(history_t* h) {
    if (history_is_disabled(h) || h->fname == NULL) {
        return -1;
    }
#ifndef _WIN32
    const size_t len = strlen(h->fname) + 6;
    char* path = mem_malloc_tp_n(h->mem, char, (ssize_t)len);
    if (path == NULL) {
        return -1;
    }
    const int written = snprintf(path, len, "%s.lock", h->fname);
    if (written < 0 || (size_t)written >= len) {
        mem_free(h->mem, path);
        return -1;
    }
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    mem_free(h->mem, path);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid()) {
            int result;
            do {
                result = flock(fd, LOCK_EX);
            } while (result != 0 && errno == EINTR);
            if (result == 0) {
                return fd;
            }
        }
        close(fd);
    }
    history_persistence_error(h);
    return -1;
#else
    return 0;
#endif
}

static void history_unlock(int fd) {
#ifndef _WIN32
    if (fd >= 0) {
        close(fd);
    }
#else
    ic_unused(fd);
#endif
}

static bool history_write_all(const history_t* h, const history_list_t* list) {
    if (h == NULL || h->fname == NULL) {
        return false;
    }
    const size_t len = strlen(h->fname) + 12;
    char* temporary = mem_malloc_tp_n(h->mem, char, (ssize_t)len);
    if (temporary == NULL) {
        return false;
    }
    const int written = snprintf(temporary, len, "%s.tmp.XXXXXX", h->fname);
    if (written < 0 || (size_t)written >= len) {
        mem_free(h->mem, temporary);
        return false;
    }
#ifndef _WIN32
    int fd = mkstemp(temporary);
    FILE* f = fd < 0 ? NULL : fdopen(fd, "w");
    if (fd >= 0 && f == NULL) {
        close(fd);
    }
#else
    FILE* f = NULL;
    if (_mktemp_s(temporary, len) == 0) {
        f = fopen(temporary, "w");
    }
#endif
    bool ok = f != NULL;
    stringbuf_t* sbuf = sbuf_new(h->mem);
    if (sbuf == NULL) {
        ok = false;
    }
    for (ssize_t i = 0; ok && i < list->count; i++) {
        ok = history_write_record(&list->entries[i], f, sbuf);
    }
    sbuf_free(sbuf);
    if (f != NULL) {
        if (fflush(f) != 0) {
            ok = false;
        }
#ifndef _WIN32
        if (ok && fsync(fileno(f)) != 0) {
            ok = false;
        }
#endif
        if (!history_close_stream(f)) {
            ok = false;
        }
    }
    if (ok) {
#ifndef _WIN32
        ok = rename(temporary, h->fname) == 0;
#else
        ok = MoveFileExA(temporary, h->fname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
             0;
#endif
    }
    if (!ok) {
        (void)remove(temporary);
        history_persistence_error((history_t*)h);
    }
    mem_free(h->mem, temporary);
    return ok;
}

ic_private void history_begin_edit(history_t* h) {
    if (history_is_disabled(h)) {
        return;
    }
    mem_free(h->mem, h->pending);
    h->pending = mem_strdup(h->mem, "");
}

ic_private void history_end_edit(history_t* h, const char* entry) {
    if (h == NULL) {
        return;
    }
    mem_free(h->mem, h->pending);
    h->pending = NULL;
    if (h->auto_add && entry != NULL && strlen(entry) > 1) {
        (void)history_push(h, entry);
    }
}

ic_private bool history_update(history_t* h, const char* entry) {
    if (history_is_disabled(h) || entry == NULL) {
        return false;
    }
    if (h->pending != NULL) {
        char* copy = history_entry_dup_trimmed(h->mem, entry);
        if (copy == NULL) {
            return false;
        }
        mem_free(h->mem, h->pending);
        h->pending = copy;
        return true;
    }
    int fd = history_lock(h);
    if (fd < 0) {
        return false;
    }
    bool ok = history_update_unlocked(h, entry);
    if (!ok) {
        history_persistence_error(h);
    }
    history_unlock(fd);
    return ok;
}

ic_private bool history_push_with_metadata(history_t* h, const char* entry,
                                           const ic_history_metadata_t* metadata,
                                           size_t metadata_count) {
    if (entry == NULL) {
        return false;
    }
    int fd = history_lock(h);
    if (fd < 0) {
        return false;
    }
    bool ok = history_push_with_metadata_unlocked(h, entry, metadata, metadata_count);
    if (!ok) {
        history_persistence_error(h);
    }
    history_unlock(fd);
    return ok;
}

ic_private void history_remove_last(history_t* h) {
    if (h == NULL) {
        return;
    }
    if (h->pending != NULL) {
        mem_free(h->mem, h->pending);
        h->pending = NULL;
        return;
    }
    int fd = history_lock(h);
    if (fd < 0) {
        return;
    }
    history_remove_last_unlocked(h);
    history_unlock(fd);
}

ic_private void history_clear(history_t* h) {
    if (h == NULL) {
        return;
    }
    mem_free(h->mem, h->pending);
    h->pending = NULL;
    int fd = history_lock(h);
    if (fd < 0) {
        return;
    }
    history_clear_unlocked(h);
    history_unlock(fd);
}

ic_private void history_load(history_t* h) {
    int fd = history_lock(h);
    if (fd < 0) {
        return;
    }
    history_list_t list;
    history_list_init(&list);
    if (history_collect_disk_entries(h, &list, true)) {
        (void)history_write_all(h, &list);
    } else {
        history_persistence_error(h);
    }
    history_list_free(h, &list);
    history_unlock(fd);
}

ic_private void history_save(const history_t* h) {
    ic_unused(h);
}
