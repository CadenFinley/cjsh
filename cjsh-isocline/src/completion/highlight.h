/*
  highlight.h

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

#ifndef IC_HIGHLIGHT_H
#define IC_HIGHLIGHT_H

#include "attr.h"
#include "bbcode.h"
#include "common.h"
#include "isocline.h"

//-------------------------------------------------------------
// Syntax highlighting
//-------------------------------------------------------------

ic_private void highlight(alloc_t* mem, bbcode_t* bb, const char* s, attrbuf_t* attrs,
                          ic_highlight_fun_t* highlighter, void* arg);
ic_private void highlight_match_braces(const char* s, attrbuf_t* attrs, ssize_t cursor_pos,
                                       const char* braces, attr_t match_attr, attr_t error_attr);
ic_private ssize_t find_matching_brace(const char* s, ssize_t cursor_pos, const char* braces,
                                       bool* is_balanced);

#endif  // IC_HIGHLIGHT_H
