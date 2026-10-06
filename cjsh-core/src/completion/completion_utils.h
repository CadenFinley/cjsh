/*
  completion_utils.h

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

#ifndef CJSH_CORE_SRC_COMPLETION_COMPLETION_UTILS_H
#define CJSH_CORE_SRC_COMPLETION_COMPLETION_UTILS_H

#include <cstddef>
#include <string>
#include <vector>

namespace completion_utils {

std::string quote_path_if_needed(const std::string& path);
std::string unquote_path(const std::string& path);
std::vector<std::string> tokenize_command_line(const std::string& line);
size_t find_last_unquoted_space(const std::string& str);
std::string normalize_for_comparison(const std::string& value);

bool starts_with_case_insensitive(const std::string& str, const std::string& prefix);
bool starts_with_case_sensitive(const std::string& str, const std::string& prefix);
bool matches_completion_prefix(const std::string& str, const std::string& prefix);
bool equals_completion_token(const std::string& value, const std::string& target);

std::string sanitize_job_command_summary(const std::string& command);

}  // namespace completion_utils

#endif  // CJSH_CORE_SRC_COMPLETION_COMPLETION_UTILS_H
