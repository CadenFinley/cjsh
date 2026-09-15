/*
  status_line.h

  This file is part of cjsh, CJ's Shell

  MIT License

  Copyright (c) 2026 Caden Finley

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

#ifndef CJSH_CORE_SRC_CORE_STATUS_LINE_H
#define CJSH_CORE_SRC_CORE_STATUS_LINE_H

#include <cstddef>
#include <string>

namespace status_line {

const char* create_below_syntax_message(const char* input_buffer, void* user_data);
// Explicit byte-offset variant for callers without an active line editor.
const char* create_below_syntax_message_at_cursor(const char* input_buffer, size_t cursor_pos);

// Temporarily replace the normal status content with operation feedback.
void set_transient_status_message(const std::string& message);
void clear_transient_status_message();

void set_user_status_callback_function(const std::string& function_name);
void clear_user_status_callback_function();
std::string get_user_status_callback_function();

}  // namespace status_line

#endif  // CJSH_CORE_SRC_CORE_STATUS_LINE_H
