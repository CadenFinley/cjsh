/*
  undo.h

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

#ifndef IC_UNDO_H
#define IC_UNDO_H

#include "common.h"

//-------------------------------------------------------------
// Edit state
//-------------------------------------------------------------
struct editstate_s;
typedef struct editstate_s editstate_t;

ic_private void editstate_init(editstate_t** es);
ic_private void editstate_done(alloc_t* mem, editstate_t** es);
ic_private bool editstate_capture(alloc_t* mem, editstate_t** es, const char* input, ssize_t pos);
ic_private void editstate_forget(alloc_t* mem, editstate_t** es);
ic_private bool editstate_restore(alloc_t* mem, editstate_t** es, const char** input,
                                  ssize_t* pos);  // caller needs to free input

#endif  // IC_UNDO_H
