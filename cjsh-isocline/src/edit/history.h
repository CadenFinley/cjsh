/*
  history.h

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

#ifndef IC_HISTORY_H
#define IC_HISTORY_H

#include <sys/stat.h>
#include <time.h>

#include "common.h"

//-------------------------------------------------------------
// History
//-------------------------------------------------------------

struct history_s;
typedef struct history_s history_t;

typedef struct history_metadata_s {
    char* key;
    char* value;
} history_metadata_t;

typedef struct history_entry_s {
    char* command;
    history_metadata_t* metadata;
    ssize_t metadata_count;
    ssize_t metadata_capacity;
} history_entry_t;

typedef struct history_snapshot_s {
    struct stat file_status;
    bool has_file_status;
    bool loaded;
    bool dedup;
    bool disabled;
    bool had_pending;
    bool allow_duplicates;
    size_t directory_revision;
    ssize_t max_entries;
    history_entry_t* entries;
    ssize_t count;
    ssize_t capacity;
} history_snapshot_t;

ic_private history_t* history_new(alloc_t* mem);
ic_private void history_free(history_t* h);
ic_private void history_clear(history_t* h);
ic_private bool history_enable_duplicates(history_t* h, bool enable);
ic_private bool history_set_fuzzy_case_sensitive(history_t* h, bool enable);
ic_private bool history_is_fuzzy_case_sensitive(const history_t* h);
ic_private bool history_enable_directory(history_t* h, bool enable);
ic_private bool history_directory_is_enabled(const history_t* h);
ic_private bool history_enable_directory_subdirs(history_t* h, bool enable);
ic_private bool history_directory_subdirs_is_enabled(const history_t* h);
ic_private bool history_set_directory(history_t* h, const char* directory);
ic_private bool history_matches_directory(const history_t* h, const char* directory);
ic_private ssize_t history_count(const history_t* h);

ic_private void history_load_from(history_t* h, const char* fname, long max_entries);
ic_private void history_load(history_t* h);
ic_private void history_save(const history_t* h);

ic_private bool history_push(history_t* h, const char* entry);
ic_private bool history_push_with_metadata(history_t* h, const char* entry,
                                           const ic_history_metadata_t* metadata,
                                           size_t metadata_count);
ic_private void history_begin_edit(history_t* h);
ic_private void history_end_edit(history_t* h, const char* entry);
ic_private bool history_enable_auto_add(history_t* h, bool enable);
ic_private bool history_update(history_t* h, const char* entry);
ic_private const char* history_get(const history_t* h, ssize_t n);
ic_private void history_remove_last(history_t* h);
ic_private const char* history_entry_get_metadata(const history_entry_t* entry, const char* key);

ic_private bool history_search(const history_t* h, ssize_t from, const char* search, bool backward,
                               ssize_t* hidx, ssize_t* hpos);

ic_private bool history_search_prefix(const history_t* h, ssize_t from, const char* prefix,
                                      bool backward, ssize_t* hidx);

ic_private bool history_snapshot_load(history_t* h, history_snapshot_t* snap, bool dedup);
ic_private bool history_snapshot_refresh(history_t* h, history_snapshot_t* snap, bool dedup);
ic_private bool history_snapshot_is_current(const history_t* h, const history_snapshot_t* snap);
ic_private void history_snapshot_free(history_t* h, history_snapshot_t* snap);
ic_private const history_entry_t* history_snapshot_get(const history_snapshot_t* snap, ssize_t n);
ic_private ssize_t history_snapshot_count(const history_snapshot_t* snap);

typedef struct history_match_s {
    ssize_t hidx;       // history index
    int score;          // match score (higher is better)
    ssize_t match_pos;  // position of first match
    ssize_t match_len;  // length of match
} history_match_t;

ic_private bool history_fuzzy_search(const history_t* h, const char* query,
                                     history_match_t* matches, ssize_t max_matches,
                                     ssize_t* match_count, bool* metadata_filter_applied);

ic_private bool history_snapshot_fuzzy_search(const history_t* h, const history_snapshot_t* snap,
                                              const char* query, history_match_t* matches,
                                              ssize_t max_matches, ssize_t* match_count,
                                              bool* metadata_filter_applied, bool case_sensitive);

ic_private bool history_fuzzy_search_with_case(const history_t* h, const char* query,
                                               history_match_t* matches, ssize_t max_matches,
                                               ssize_t* match_count, bool* metadata_filter_applied,
                                               bool case_sensitive);

#endif  // IC_HISTORY_H
