/*
  undo.c

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

#include "undo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"

//-------------------------------------------------------------
// edit state
//-------------------------------------------------------------
struct editstate_s {
    struct editstate_s* next;
    char* input;
    ssize_t input_len;
    ssize_t pos;
    // Only the newest state owns a full input buffer. Older states hold the
    // changed bytes needed to reconstruct them from the state above them.
    ssize_t prefix_len;
    ssize_t suffix_len;
    ssize_t capacity;  // newest buffer retains capacity for every older state
};

ic_private void editstate_init(editstate_t** es) {
    *es = NULL;
}

ic_private void editstate_done(alloc_t* mem, editstate_t** es) {
    while (*es != NULL) {
        editstate_t* next = (*es)->next;
        mem_free(mem, (*es)->input);
        mem_free(mem, *es);
        *es = next;
    }
    *es = NULL;
}

ic_private bool editstate_capture(alloc_t* mem, editstate_t** es, const char* input, ssize_t pos) {
    const ssize_t len = (input == NULL ? 0 : ic_strlen(input));
    if (input == NULL) {
        input = "";
    }
    editstate_t* previous = *es;
    const ssize_t maxsize = (ssize_t)(SIZE_MAX / 2);
    if (len < 0 || len == maxsize) {
        return false;  // the buffer capacity must also include a terminator
    }
    ssize_t prefix = 0;
    ssize_t suffix = 0;
    if (previous != NULL) {
        const ssize_t common = (previous->input_len < len ? previous->input_len : len);
        // Appending/deleting at the end is the usual case. Let libc compare the
        // shared prefix in bulk instead of scanning it byte by byte.
        if (memcmp(previous->input, input, (size_t)common) == 0) {
            prefix = common;
        } else {
            while (prefix < common && previous->input[prefix] == input[prefix]) {
                prefix++;
            }
            while (suffix < common - prefix &&
                   previous->input[previous->input_len - suffix - 1] == input[len - suffix - 1]) {
                suffix++;
            }
        }
    }

    // Finish every allocation before changing the old stack. A failed capture
    // must leave all previously recorded undo/redo states usable.
    editstate_t* entry = mem_zalloc_tp(mem, editstate_t);
    if (entry == NULL) {
        return false;
    }
    char* removed = NULL;
    if (previous != NULL && previous->input_len > prefix + suffix) {
        removed = mem_strndup(mem, previous->input + prefix, previous->input_len - prefix - suffix);
        if (removed == NULL) {
            mem_free(mem, entry);
            return false;
        }
    }
    ssize_t capacity = (previous == NULL ? 0 : previous->capacity);
    char* buffer = (previous == NULL ? NULL : previous->input);
    if (capacity <= len) {
        const ssize_t growth = (capacity > 0 ? capacity / 2 : 256);
        capacity = (capacity > maxsize - growth ? maxsize : capacity + growth);
        if (capacity <= len) {
            capacity = len + 1;
        }
        buffer = mem_realloc_tp(mem, char, buffer, capacity);
        if (buffer == NULL) {
            mem_free(mem, removed);
            mem_free(mem, entry);
            return false;
        }
    }
    if (previous != NULL) {
        memmove(buffer + len - suffix, buffer + previous->input_len - suffix, (size_t)suffix);
        previous->input = removed;
        previous->prefix_len = prefix;
        previous->suffix_len = suffix;
        previous->capacity = 0;
    }
    memcpy(buffer + prefix, input + prefix, (size_t)(len - prefix - suffix));
    buffer[len] = '\0';
    entry->input = buffer;
    entry->input_len = len;
    entry->capacity = capacity;
    entry->pos = pos;
    entry->next = previous;
    *es = entry;
    return true;
}

// Advance the reusable buffer to the preceding state, without allocating.
ic_private void editstate_forget(alloc_t* mem, editstate_t** es) {
    editstate_t* entry = *es;
    if (entry == NULL) {
        return;
    }
    editstate_t* next = entry->next;
    if (next != NULL) {
        const ssize_t changed_len = next->input_len - next->prefix_len - next->suffix_len;
        assert(entry->capacity > next->input_len);
        memmove(entry->input + next->prefix_len + changed_len,
                entry->input + entry->input_len - next->suffix_len, (size_t)next->suffix_len);
        if (changed_len > 0) {
            memcpy(entry->input + next->prefix_len, next->input, (size_t)changed_len);
        }
        entry->input[next->input_len] = '\0';
        mem_free(mem, next->input);
        next->input = entry->input;
        next->capacity = entry->capacity;
        next->prefix_len = 0;
        next->suffix_len = 0;
    } else {
        mem_free(mem, entry->input);
    }
    *es = next;
    mem_free(mem, entry);
}

// caller should free *input
ic_private bool editstate_restore(alloc_t* mem, editstate_t** es, const char** input,
                                  ssize_t* pos) {
    editstate_t* entry = *es;
    if (entry == NULL) {
        return false;
    }
    char* restored = entry->input;
    if (entry->next != NULL) {
        restored = mem_strndup(mem, entry->input, entry->input_len);
        if (restored == NULL) {
            return false;
        }
    } else {
        entry->input = NULL;  // transfer the last buffer directly to the caller
    }
    *input = restored;
    *pos = entry->pos;
    editstate_forget(mem, es);
    return true;
}
