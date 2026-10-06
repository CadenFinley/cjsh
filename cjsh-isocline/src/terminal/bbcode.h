/*
  bbcode.h

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

#ifndef IC_BBCODE_H
#define IC_BBCODE_H

#include <stdarg.h>

#include "attr.h"
#include "common.h"
#include "term.h"
#include "tty.h"

struct bbcode_s;
typedef struct bbcode_s bbcode_t;

ic_private bbcode_t* bbcode_new(alloc_t* mem, term_t* term);
ic_private void bbcode_free(bbcode_t* bb);

ic_private void bbcode_style_add(bbcode_t* bb, const char* style_name, attr_t attr);
ic_private void bbcode_style_def(bbcode_t* bb, const char* style_name, const char* s);
ic_private void bbcode_style_open(bbcode_t* bb, const char* fmt);
ic_private void bbcode_style_close(bbcode_t* bb, const char* fmt);
ic_private attr_t bbcode_style(bbcode_t* bb, const char* style_name);

ic_private void bbcode_print(bbcode_t* bb, const char* s);
ic_private void bbcode_println(bbcode_t* bb, const char* s);
ic_private void bbcode_printf(bbcode_t* bb, const char* fmt, ...);
ic_private void bbcode_vprintf(bbcode_t* bb, const char* fmt, va_list args);

ic_private ssize_t bbcode_column_width(bbcode_t* bb, const char* s);

// allows `attr_out == NULL`.
ic_private void bbcode_append(bbcode_t* bb, const char* s, stringbuf_t* out, attrbuf_t* attr_out);

#endif  // IC_BBCODE_H
