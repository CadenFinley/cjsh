/*
  completions.h

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

#ifndef IC_COMPLETIONS_H
#define IC_COMPLETIONS_H

#include "common.h"
#include "stringbuf.h"

//-------------------------------------------------------------
// Completions
//-------------------------------------------------------------
#define IC_COMPLETION_DISPLAY_TRUSTED_PREFIX ((char)0x1F)
#define IC_MAX_COMPLETIONS_TO_SHOW (10000)
#define IC_MAX_COMPLETIONS_TO_TRY (IC_MAX_COMPLETIONS_TO_SHOW)

// completion could not be applied
#define IC_COMP_APPLY_FAIL -1
// completion did not change buffer or cursor position
#define IC_COMP_APPLY_NOOP -2

typedef struct completions_s completions_t;

ic_private completions_t* completions_new(alloc_t* mem);
ic_private void completions_free(completions_t* cms);
ic_private void completions_clear(completions_t* cms);
ic_private bool completions_add(completions_t* cms, const char* replacement, const char* display,
                                const char* help, const char* source, ssize_t delete_before,
                                ssize_t delete_after);
ic_private ssize_t completions_count(completions_t* cms);
ic_private ssize_t completions_generate(struct ic_env_s* env, completions_t* cms, const char* input,
                                        ssize_t pos, ssize_t max);
ic_private ssize_t completions_generate_hint(struct ic_env_s* env, completions_t* cms,
                                             const char* input, ssize_t pos, ssize_t max);
ic_private void completions_sort(completions_t* cms);
ic_private void completions_set_completer(completions_t* cms, ic_completer_fun_t* completer,
                                          void* arg);
ic_private const char* completions_get_display(completions_t* cms, ssize_t index,
                                               const char** help);
ic_private const char* completions_get_replacement(completions_t* cms, ssize_t index);
ic_private const char* completions_get_source(completions_t* cms, ssize_t index);
ic_private bool completions_get_apply_range(completions_t* cms, ssize_t index, const char* input,
                                            ssize_t pos, const char** replacement,
                                            ssize_t* replacement_start, ssize_t* delete_after);
ic_private bool completions_all_sources_equal(completions_t* cms, const char* source);
ic_private const char* completions_get_hint(completions_t* cms, ssize_t index, const char** help);
ic_private void completions_get_completer(completions_t* cms, ic_completer_fun_t** completer,
                                          void** arg);

ic_private ssize_t completions_apply(completions_t* cms, ssize_t index, stringbuf_t* sbuf,
                                     ssize_t pos);
ic_private ssize_t completions_apply_longest_prefix(completions_t* cms, stringbuf_t* sbuf,
                                                    ssize_t pos);

//-------------------------------------------------------------
// Completion environment
//-------------------------------------------------------------
typedef bool(ic_completion_fun_t)(ic_env_t* env, void* funenv, const char* replacement,
                                  const char* display, const char* help, long delete_before,
                                  long delete_after);

typedef bool(ic_completion_fun_with_source_t)(ic_env_t* env, void* funenv, const char* replacement,
                                              const char* display, const char* help,
                                              const char* source, long delete_before,
                                              long delete_after);

struct ic_completion_env_s {
    ic_env_t* env;                  // the isocline environment
    bool is_hint;                   // automatic suggestion rather than explicit completion
    const char* input;              // current full input
    long cursor;                    // current cursor position
    void* arg;                      // argument given to `ic_set_completer`
    void* closure;                  // free variables for function composition
    ic_completion_fun_t* complete;  // function that adds a completion
    ic_completion_fun_with_source_t*
        complete_with_source;  // function that adds a completion with source
};

#endif  // IC_COMPLETIONS_H
