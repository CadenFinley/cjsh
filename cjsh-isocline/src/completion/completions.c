/*
  completions.c

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

#include "completions.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"
#include "env.h"
#include "isocline.h"
#include "stringbuf.h"

//-------------------------------------------------------------
// Completions
//-------------------------------------------------------------

typedef struct completion_s {
    const char* replacement;
    const char* display;
    const char* help;
    const char* source;
    ssize_t delete_before;
    ssize_t delete_after;
} completion_t;

struct completions_s {
    ic_completer_fun_t* completer;
    void* completer_arg;
    ssize_t completer_max;
    ssize_t count;
    ssize_t len;
    completion_t* elems;
    const char** replacement_index;  // borrowed strings; independent of elems reallocations
    size_t replacement_index_capacity;
    ssize_t indexed_count;
    alloc_t* mem;
    char* input;
    ssize_t input_pos;
};

static void default_filename_completer(ic_completion_env_t* cenv, const char* prefix);

ic_private completions_t* completions_new(alloc_t* mem) {
    completions_t* cms = mem_zalloc_tp(mem, completions_t);
    if (cms == NULL) {
        return NULL;
    }
    cms->mem = mem;
    cms->completer = &default_filename_completer;
    return cms;
}

ic_private void completions_free(completions_t* cms) {
    if (cms == NULL) {
        return;
    }
    completions_clear(cms);
    mem_free(cms->mem, cms->replacement_index);
    if (cms->elems != NULL) {
        mem_free(cms->mem, cms->elems);
        cms->elems = NULL;
        cms->count = 0;
        cms->len = 0;
    }
    mem_free(cms->mem, cms);  // free ourselves
}

// Escape bbcode control characters so completion metadata cannot inject styles.
static char* completions_escape_bbcode(alloc_t* mem, const char* text) {
    if (text != NULL && *text == IC_COMPLETION_DISPLAY_TRUSTED_PREFIX) {
        return mem_strdup(mem, text + 1);
    }
    if (text == NULL) {
        return NULL;
    }
    const ssize_t len = ic_strlen(text);
    if (len <= 0) {
        return mem_strdup(mem, text);
    }

    ssize_t extra = 0;
    for (ssize_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '[' || ch == '\\') {
            extra++;
        }
    }

    if (extra == 0) {
        return mem_strdup(mem, text);
    }

    char* escaped = mem_malloc_tp_n(mem, char, len + extra + 1);
    if (escaped == NULL) {
        return NULL;
    }

    char* dest = escaped;
    for (ssize_t i = 0; i < len; i++) {
        unsigned char uch = (unsigned char)text[i];
        char ch = (char)uch;
        if (uch < 0x20 || uch == 0x7F) {
            ch = ' ';
        }
        if (ch == '[' || ch == '\\') {
            *dest++ = '\\';
        }
        *dest++ = ch;
    }
    *dest = '\0';
    return escaped;
}

ic_private void completions_clear(completions_t* cms) {
    if (cms->count > 0 && cms->replacement_index != NULL) {
        memset(cms->replacement_index, 0,
               cms->replacement_index_capacity * sizeof(*cms->replacement_index));
    }
    cms->indexed_count = 0;
    while (cms->count > 0) {
        completion_t* cm = cms->elems + cms->count - 1;
        mem_free(cms->mem, cm->display);
        mem_free(cms->mem, cm->replacement);
        mem_free(cms->mem, cm->help);
        mem_free(cms->mem, cm->source);
        memset(cm, 0, sizeof(*cm));
        cms->count--;
    }
    mem_free(cms->mem, cms->input);
    cms->input = NULL;
    cms->input_pos = 0;
}

static bool completions_set_entry(completions_t* cms, completion_t* cm, const char* replacement,
                                  const char* display, const char* help, const char* source,
                                  ssize_t delete_before, ssize_t delete_after) {
    char* new_replacement = NULL;
    char* new_display = NULL;
    char* new_help = NULL;
    char* new_source = NULL;

    if (replacement != NULL) {
        new_replacement = mem_strdup(cms->mem, replacement);
        if (new_replacement == NULL) {
            goto fail;
        }
    }
    const char* display_text = (display != NULL ? display : replacement);
    if (display_text != NULL) {
        new_display = completions_escape_bbcode(cms->mem, display_text);
        if (new_display == NULL) {
            goto fail;
        }
    }
    if (help != NULL) {
        new_help = completions_escape_bbcode(cms->mem, help);
        if (new_help == NULL) {
            goto fail;
        }
    }
    if (source != NULL) {
        new_source = completions_escape_bbcode(cms->mem, source);
        if (new_source == NULL) {
            goto fail;
        }
    }

    mem_free(cms->mem, cm->replacement);
    mem_free(cms->mem, cm->display);
    mem_free(cms->mem, cm->help);
    mem_free(cms->mem, cm->source);

    cm->replacement = (replacement != NULL ? new_replacement : NULL);
    cm->display = new_display;
    cm->help = (help != NULL ? new_help : NULL);
    cm->source = (source != NULL ? new_source : NULL);
    cm->delete_before = delete_before;
    cm->delete_after = delete_after;
    return true;

fail:
    mem_free(cms->mem, new_replacement);
    mem_free(cms->mem, new_display);
    mem_free(cms->mem, new_help);
    mem_free(cms->mem, new_source);
    return false;
}

static bool completions_push(completions_t* cms, const char* replacement, const char* display,
                             const char* help, const char* source, ssize_t delete_before,
                             ssize_t delete_after) {
    if (cms->count >= cms->len) {
        ssize_t newlen = (cms->len <= 0 ? 32 : cms->len * 2);
        completion_t* newelems = mem_realloc_tp(cms->mem, completion_t, cms->elems, newlen);
        if (newelems == NULL) {
            return false;
        }
        cms->elems = newelems;
        cms->len = newlen;
    }
    assert(cms->count < cms->len);
    completion_t* cm = cms->elems + cms->count;
    memset(cm, 0, sizeof(*cm));
    if (!completions_set_entry(cms, cm, replacement, display, help, source, delete_before,
                               delete_after)) {
        memset(cm, 0, sizeof(*cm));
        return false;
    }
    cms->count++;
    return true;
}

ic_private ssize_t completions_count(completions_t* cms) {
    return cms->count;
}

static size_t completion_hash(const char* replacement) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char* p = (const unsigned char*)replacement; *p != 0; ++p) {
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    }
    return (size_t)hash;
}

static size_t completion_index_slot(const char** index, size_t capacity, const char* replacement) {
    size_t slot = completion_hash(replacement) & (capacity - 1);
    while (index[slot] != NULL && strcmp(index[slot], replacement) != 0) {
        slot = (slot + 1) & (capacity - 1);
    }
    return slot;
}

// Keep at least half the table empty. Small hint generations need no table, and
// allocation failure falls back to the original scan without losing completions.
static bool completions_prepare_index(completions_t* cms) {
    if (cms->count < 32 && cms->replacement_index == NULL) {
        return false;
    }
    size_t capacity = cms->replacement_index_capacity;
    if (capacity > (size_t)cms->count * 2 && cms->indexed_count == cms->count) {
        return true;
    }
    if (capacity == 0) {
        capacity = 128;
    }
    while (capacity / 2 <= (size_t)cms->count) {
        if (capacity > (SIZE_MAX / 2) / (2 * sizeof(*cms->replacement_index))) {
            return false;
        }
        capacity *= 2;
    }
    if (capacity != cms->replacement_index_capacity) {
        const char** index = mem_zalloc_tp_n(cms->mem, const char*, capacity);
        if (index == NULL) {
            return false;
        }
        mem_free(cms->mem, cms->replacement_index);
        cms->replacement_index = index;
        cms->replacement_index_capacity = capacity;
    } else {
        memset(cms->replacement_index, 0, capacity * sizeof(*cms->replacement_index));
    }
    for (ssize_t i = 0; i < cms->count; ++i) {
        const char* replacement = cms->elems[i].replacement;
        if (replacement != NULL) {
            size_t slot = completion_index_slot(cms->replacement_index, capacity, replacement);
            cms->replacement_index[slot] = replacement;
        }
    }
    cms->indexed_count = cms->count;
    return true;
}

ic_private bool completions_add(completions_t* cms, const char* replacement, const char* display,
                                const char* help, const char* source, ssize_t delete_before,
                                ssize_t delete_after) {
    if (cms->completer_max <= 0) {
        return false;
    }

    cms->completer_max--;

    bool indexed = false;
    size_t slot = 0;
    if (replacement != NULL) {
        indexed = completions_prepare_index(cms);
        if (indexed) {
            slot = completion_index_slot(cms->replacement_index, cms->replacement_index_capacity,
                                         replacement);
            if (cms->replacement_index[slot] != NULL) {
                return true;
            }
        } else {
            for (ssize_t i = 0; i < cms->count; i++) {
                const completion_t* existing = cms->elems + i;
                if (existing->replacement != NULL &&
                    strcmp(replacement, existing->replacement) == 0) {
                    return true;
                }
            }
        }
    }

    if (!completions_push(cms, replacement, display, help, source, delete_before, delete_after)) {
        cms->completer_max++;
        return false;
    }
    if (indexed) {
        cms->replacement_index[slot] = cms->elems[cms->count - 1].replacement;
        cms->indexed_count = cms->count;
    }
    return true;
}

static completion_t* completions_get(completions_t* cms, ssize_t index) {
    if (index < 0 || cms->count <= 0 || index >= cms->count) {
        return NULL;
    }
    return &cms->elems[index];
}

ic_private const char* completions_get_display(completions_t* cms, ssize_t index,
                                               const char** help) {
    if (help != NULL) {
        *help = NULL;
    }
    if (cms == NULL) {
        return NULL;
    }

    completion_t* cm = completions_get(cms, index);
    if (cm == NULL) {
        return NULL;
    }
    if (help != NULL) {
        *help = cm->help;
    }
    return (cm->display != NULL ? cm->display : cm->replacement);
}

ic_private const char* completions_get_replacement(completions_t* cms, ssize_t index) {
    completion_t* cm = completions_get(cms, index);
    if (cm == NULL) {
        return NULL;
    }
    return cm->replacement;
}

ic_private const char* completions_get_source(completions_t* cms, ssize_t index) {
    completion_t* cm = completions_get(cms, index);
    if (cm == NULL) {
        return NULL;
    }
    return cm->source;
}

ic_private bool completions_all_sources_equal(completions_t* cms, const char* source) {
    if (cms == NULL || source == NULL || cms->count <= 0) {
        return false;
    }
    for (ssize_t i = 0; i < cms->count; ++i) {
        completion_t* cm = completions_get(cms, i);
        if (cm == NULL || cm->source == NULL) {
            return false;
        }
        if (strcmp(cm->source, source) != 0) {
            return false;
        }
    }
    return true;
}

static const char* completion_get_hint_text(completion_t* cm) {
    if (cm == NULL) {
        return NULL;
    }
    ssize_t len = ic_strlen(cm->replacement);
    if (len < cm->delete_before) {
        return NULL;
    }
    const char* hint = (cm->replacement + cm->delete_before);
    if (*hint == 0 || utf8_is_cont((uint8_t)(*hint))) {
        return NULL;  // utf8 boundary?
    }
    return hint;
}

static ssize_t completion_matching_hint_after_cursor(const char* hint, const char* input,
                                                     ssize_t pos) {
    if (hint == NULL || input == NULL || pos < 0) {
        return 0;
    }

    ssize_t input_len = ic_strlen(input);
    if (pos > input_len) {
        return 0;
    }

    ssize_t hint_len = ic_strlen(hint);
    ssize_t after_len = input_len - pos;
    if (hint_len <= 0 || after_len <= 0) {
        return 0;
    }

    ssize_t max_overlap = hint_len < after_len ? hint_len : after_len;
    const char* after = input + pos;
    for (ssize_t overlap = max_overlap; overlap > 0; overlap--) {
        if (strncmp(hint, after, to_size_t(overlap)) == 0) {
            return overlap;
        }
    }
    return 0;
}

ic_private bool completions_get_apply_range(completions_t* cms, ssize_t index, const char* input,
                                            ssize_t pos, const char** replacement,
                                            ssize_t* replacement_start, ssize_t* delete_after) {
    if (replacement != NULL) {
        *replacement = NULL;
    }
    if (replacement_start != NULL) {
        *replacement_start = 0;
    }
    if (delete_after != NULL) {
        *delete_after = 0;
    }

    if (cms == NULL) {
        return false;
    }

    completion_t* cm = completions_get(cms, index);
    if (cm == NULL || cm->replacement == NULL || input == NULL || pos < 0) {
        return false;
    }

    ssize_t input_len = ic_strlen(input);
    if (pos > input_len) {
        return false;
    }

    ssize_t start = pos - cm->delete_before;
    if (start < 0) {
        start = 0;
    }

    ssize_t effective_delete_after = cm->delete_after;
    if (effective_delete_after == 0) {
        effective_delete_after =
            completion_matching_hint_after_cursor(completion_get_hint_text(cm), input, pos);
    }
    if (effective_delete_after < 0) {
        effective_delete_after = 0;
    }
    if (pos + effective_delete_after > input_len) {
        effective_delete_after = input_len - pos;
    }

    if (replacement != NULL) {
        *replacement = cm->replacement;
    }
    if (replacement_start != NULL) {
        *replacement_start = start;
    }
    if (delete_after != NULL) {
        *delete_after = effective_delete_after;
    }
    return true;
}

ic_private const char* completions_get_hint(completions_t* cms, ssize_t index, const char** help) {
    if (help != NULL) {
        *help = NULL;
    }
    completion_t* cm = completions_get(cms, index);
    const char* hint = completion_get_hint_text(cm);
    if (hint == NULL) {
        return NULL;
    }
    if (completion_matching_hint_after_cursor(hint, cms->input, cms->input_pos) > 0) {
        return NULL;
    }
    if (help != NULL) {
        *help = cm->help;
    }
    return hint;
}

ic_private void completions_set_completer(completions_t* cms, ic_completer_fun_t* completer,
                                          void* arg) {
    cms->completer = completer;
    cms->completer_arg = arg;
}

ic_private void completions_get_completer(completions_t* cms, ic_completer_fun_t** completer,
                                          void** arg) {
    *completer = cms->completer;
    *arg = cms->completer_arg;
}

ic_public const char* ic_completion_input(ic_completion_env_t* cenv, long* cursor) {
    if (cenv == NULL) {
        return NULL;
    }
    if (cursor != NULL) {
        *cursor = cenv->cursor;
    }
    return cenv->input;
}

ic_public void* ic_completion_arg(const ic_completion_env_t* cenv) {
    return (cenv == NULL ? NULL : cenv->env->completions->completer_arg);
}

ic_public bool ic_completion_is_hint(const ic_completion_env_t* cenv) {
    return cenv != NULL && cenv->is_hint;
}

ic_public bool ic_has_completions(const ic_completion_env_t* cenv) {
    return (cenv == NULL ? false : cenv->env->completions->count > 0);
}

ic_public bool ic_stop_completing(const ic_completion_env_t* cenv) {
    return (cenv == NULL ? true : cenv->env->completions->completer_max <= 0);
}

static ssize_t completion_apply(completion_t* cm, stringbuf_t* sbuf, ssize_t pos) {
    if (cm == NULL) {
        return IC_COMP_APPLY_FAIL;
    }
    debug_msg("completion: apply: %s at %zd\n", cm->replacement, pos);
    ssize_t start = pos - cm->delete_before;
    if (start < 0) {
        start = 0;
    }
    ssize_t delete_after = cm->delete_after;
    if (delete_after == 0) {
        delete_after = completion_matching_hint_after_cursor(completion_get_hint_text(cm),
                                                             sbuf_string(sbuf), pos);
    }
    ssize_t n = cm->delete_before + delete_after;
    if (ic_strlen(cm->replacement) == n &&
        strncmp(sbuf_string_at(sbuf, start), cm->replacement, to_size_t(n)) == 0) {
        if (delete_after > 0) {
            return start + n;
        }
        return IC_COMP_APPLY_NOOP;
    } else {
        sbuf_delete_from_to(sbuf, start, pos + delete_after);
        return sbuf_insert_at(sbuf, cm->replacement, start);
    }
}

ic_private ssize_t completions_apply(completions_t* cms, ssize_t index, stringbuf_t* sbuf,
                                     ssize_t pos) {
    completion_t* cm = completions_get(cms, index);
    return completion_apply(cm, sbuf, pos);
}

ic_private void completions_sort(completions_t* cms) {
    if (cms == NULL) {
        return;
    }
    // preserve insertion order; intentionally no sorting
}

#define IC_MAX_PREFIX (256)

// find longest common prefix (considering full replacements) and complete with that.
ic_private ssize_t completions_apply_longest_prefix(completions_t* cms, stringbuf_t* sbuf,
                                                    ssize_t pos) {
    if (cms->count <= 1) {
        return completions_apply(cms, 0, sbuf, pos);
    }

    if (pos < 0) {
        return IC_COMP_APPLY_FAIL;
    }

    size_t prefix_len = (size_t)pos;
    if (prefix_len >= IC_MAX_PREFIX) {
        return IC_COMP_APPLY_FAIL;  // avoid overrunning our working buffer
    }

    size_t buffer_len = (size_t)sbuf_len(sbuf);
    if (prefix_len > buffer_len) {
        return IC_COMP_APPLY_FAIL;
    }

    char* original_prefix = NULL;
    if (prefix_len > 0) {
        original_prefix = (char*)malloc(prefix_len + 1);
        if (original_prefix == NULL) {
            return IC_COMP_APPLY_FAIL;
        }
        memcpy(original_prefix, sbuf_string(sbuf), prefix_len);
        original_prefix[prefix_len] = '\0';
    }

    char common[IC_MAX_PREFIX + 1] = {0};
    size_t common_len = 0;
    bool common_initialized = false;

    for (ssize_t i = 0; i < cms->count; i++) {
        completion_t* cm = completions_get(cms, i);
        if (cm == NULL || cm->replacement == NULL) {
            continue;
        }
        if (cm->delete_before < 0) {
            continue;
        }
        size_t delete_before = (size_t)cm->delete_before;
        if (delete_before > prefix_len) {
            continue;
        }

        size_t keep_len = prefix_len - delete_before;
        const char* replacement = cm->replacement;
        size_t replacement_len = ic_strlen(replacement);
        size_t final_len = keep_len + replacement_len;
        if (final_len <= prefix_len) {
            continue;  // nothing new to add
        }

        size_t capped_final_len = (final_len > IC_MAX_PREFIX ? IC_MAX_PREFIX : final_len);
        char final_prefix[IC_MAX_PREFIX + 1];
        size_t idx = 0;

        if (keep_len > 0 && original_prefix != NULL) {
            size_t copy_len = keep_len;
            if (copy_len > capped_final_len) {
                copy_len = capped_final_len;
            }
            memcpy(final_prefix, original_prefix, copy_len);
            idx = copy_len;
        }

        if (idx < capped_final_len) {
            size_t remaining = capped_final_len - idx;
            size_t repl_copy = (replacement_len > remaining ? remaining : replacement_len);
            memcpy(final_prefix + idx, replacement, repl_copy);
            idx += repl_copy;
        }

        final_prefix[idx] = '\0';

        if (prefix_len > 0 && original_prefix != NULL) {
            if (memcmp(final_prefix, original_prefix, prefix_len) != 0) {
                continue;
            }
        }

        if (!common_initialized) {
            memcpy(common, final_prefix, idx + 1);
            common_len = idx;
            common_initialized = true;
        } else {
            size_t limit = (common_len < idx ? common_len : idx);
            size_t new_common_len = 0;
            while (new_common_len < limit &&
                   common[new_common_len] == final_prefix[new_common_len]) {
                new_common_len++;
            }
            common_len = new_common_len;
            common[common_len] = '\0';
        }

        if (common_len <= prefix_len) {
            common_len = prefix_len;
            break;
        }
    }

    if (original_prefix != NULL) {
        free(original_prefix);
    }

    if (!common_initialized || common_len <= prefix_len) {
        return IC_COMP_APPLY_NOOP;
    }

    size_t insert_len = common_len - prefix_len;
    if (insert_len > IC_MAX_PREFIX) {
        insert_len = IC_MAX_PREFIX;
    }

    char insert_text[IC_MAX_PREFIX + 1];
    for (size_t j = 0; j < insert_len; j++) {
        insert_text[j] = common[prefix_len + j];
    }
    insert_text[insert_len] = '\0';

    completion_t cprefix;
    memset(&cprefix, 0, sizeof(cprefix));
    cprefix.delete_before = 0;
    cprefix.replacement = insert_text;
    cprefix.delete_after = ic_count_end_overlap(insert_text, sbuf_string_at(sbuf, pos));

    ssize_t newpos = completion_apply(&cprefix, sbuf, pos);
    if (newpos < 0) {
        return newpos;
    }

    for (ssize_t i = 0; i < cms->count; i++) {
        completion_t* cm = completions_get(cms, i);
        if (cm != NULL) {
            cm->delete_before += (ssize_t)insert_len;
        }
    }

    return newpos;
}

//-------------------------------------------------------------
// Completer functions
//-------------------------------------------------------------

ic_public bool ic_add_completions(ic_completion_env_t* cenv, const char* prefix,
                                  const char** completions) {
    for (const char** pc = completions; *pc != NULL; pc++) {
        if (ic_istarts_with(*pc, prefix) && (!ic_add_completion_ex(cenv, *pc, NULL, NULL))) {
            return false;
        }
    }
    return true;
}

ic_public bool ic_add_completion(ic_completion_env_t* cenv, const char* replacement) {
    return ic_add_completion_ex(cenv, replacement, NULL, NULL);
}

ic_public bool ic_add_completion_ex(ic_completion_env_t* cenv, const char* replacement,
                                    const char* display, const char* help) {
    return ic_add_completion_prim(cenv, replacement, display, help, 0, 0);
}

ic_public bool ic_add_completion_ex_with_source(ic_completion_env_t* cenv, const char* replacement,
                                                const char* display, const char* help,
                                                const char* source) {
    return ic_add_completion_prim_with_source(cenv, replacement, display, help, source, 0, 0);
}

ic_public bool ic_add_completion_prim(ic_completion_env_t* cenv, const char* replacement,
                                      const char* display, const char* help, long delete_before,
                                      long delete_after) {
    return (*cenv->complete)(cenv->env, cenv->closure, replacement, display, help, delete_before,
                             delete_after);
}

ic_public bool ic_add_completion_prim_with_source(ic_completion_env_t* cenv,
                                                  const char* replacement, const char* display,
                                                  const char* help, const char* source,
                                                  long delete_before, long delete_after) {
    return (*cenv->complete_with_source)(cenv->env, cenv->closure, replacement, display, help,
                                         source, delete_before, delete_after);
}

static bool prim_add_completion(ic_env_t* env, void* funenv, const char* replacement,
                                const char* display, const char* help, long delete_before,
                                long delete_after) {
    ic_unused(funenv);
    return completions_add(env->completions, replacement, display, help, NULL, delete_before,
                           delete_after);
}

static bool prim_add_completion_with_source(ic_env_t* env, void* funenv, const char* replacement,
                                            const char* display, const char* help,
                                            const char* source, long delete_before,
                                            long delete_after) {
    ic_unused(funenv);
    return completions_add(env->completions, replacement, display, help, source, delete_before,
                           delete_after);
}

ic_public void ic_set_default_completer(ic_completer_fun_t* completer, void* arg) {
    ic_env_t* env = ic_get_env();
    if (env == NULL) {
        return;
    }
    completions_set_completer(env->completions, completer, arg);
}

static ssize_t completions_generate_internal(struct ic_env_s* env, completions_t* cms,
                                             const char* input, ssize_t pos, ssize_t max,
                                             bool is_hint) {
    completions_clear(cms);
    if (cms->completer == NULL || input == NULL || ic_strlen(input) < pos) {
        return 0;
    }

    // set up env
    ic_completion_env_t cenv;
    cenv.env = env;
    cenv.is_hint = is_hint;
    cenv.input = input, cenv.cursor = (long)pos;
    cenv.arg = cms->completer_arg;
    cenv.complete = &prim_add_completion;
    cenv.complete_with_source = &prim_add_completion_with_source;
    cenv.closure = NULL;
    const char* prefix_alloc = mem_strndup(cms->mem, input, pos);
    const char* prefix = prefix_alloc;
    if (prefix == NULL) {
        if (pos != 0) {
            return 0;
        }
        prefix = "";
    }
    cms->completer_max = max;
    cms->input = mem_strdup(cms->mem, input);
    cms->input_pos = pos;

    // and complete
    cms->completer(&cenv, prefix);

    // restore
    if (prefix_alloc != NULL) {
        mem_free(cms->mem, prefix_alloc);
    }
    return completions_count(cms);
}

ic_private ssize_t completions_generate(struct ic_env_s* env, completions_t* cms, const char* input,
                                        ssize_t pos, ssize_t max) {
    return completions_generate_internal(env, cms, input, pos, max, false);
}

ic_private ssize_t completions_generate_hint(struct ic_env_s* env, completions_t* cms,
                                             const char* input, ssize_t pos, ssize_t max) {
    return completions_generate_internal(env, cms, input, pos, max, true);
}

// The default completer is no completion is set
static void default_filename_completer(ic_completion_env_t* cenv, const char* prefix) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    ic_complete_filename(cenv, prefix, sep, ".", NULL);
}
