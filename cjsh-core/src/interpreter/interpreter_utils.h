/*
  interpreter_utils.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_INTERPRETER_UTILS_H
#define CJSH_CORE_SRC_INTERPRETER_INTERPRETER_UTILS_H

#include <cstddef>
#include <string>
#include <vector>

struct SignalProcessingResult;

namespace shell_script_interpreter::detail {

std::string trim(const std::string& s);
size_t find_inline_comment_start(const std::string& s, size_t start = 0,
                                 size_t end = std::string::npos);
std::string strip_inline_comment(const std::string& s);
std::string process_line_for_validation(const std::string& line);
std::vector<std::string> split_ampersand(const std::string& s);
bool is_readable_file(const std::string& path);

bool is_control_flow_exit_code(int code);
int pending_signal_exit_code(const SignalProcessingResult& result);
bool should_skip_line(const std::string& line);
bool contains_token(const std::string& text, const std::string& token);

}  // namespace shell_script_interpreter::detail

#endif  // CJSH_CORE_SRC_INTERPRETER_INTERPRETER_UTILS_H
