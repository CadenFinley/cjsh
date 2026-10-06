/*
  isocline_typeahead.h

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

#ifndef IC_TYPEAHEAD_H
#define IC_TYPEAHEAD_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "stringbuf.h"

ic_private void ic_typeahead_filter_escape_sequences_into(const char* input, size_t input_len,
                                                          stringbuf_t* output);
ic_private void ic_typeahead_normalize_line_edit_sequences_into(const char* input, size_t input_len,
                                                                stringbuf_t* output);
ic_private bool ic_typeahead_ingest_raw_input(const uint8_t* data, size_t length);
ic_private void ic_typeahead_prepare_for_readline(ic_env_t* env);
ic_private const char* ic_typeahead_pending_initial_input(ic_env_t* env);
ic_private ssize_t ic_typeahead_pending_initial_input_len(ic_env_t* env);
ic_private ssize_t ic_typeahead_pending_raw_byte_count(ic_env_t* env);

#endif
