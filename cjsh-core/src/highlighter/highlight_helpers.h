/*
  highlight_helpers.h

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

#ifndef CJSH_CORE_SRC_HIGHLIGHTER_HIGHLIGHT_HELPERS_H
#define CJSH_CORE_SRC_HIGHLIGHTER_HIGHLIGHT_HELPERS_H

#include <string>
#include <vector>

#include "isocline.h"

namespace highlight_helpers {

void highlight_quotes_and_variables(ic_highlight_env_t* henv, const char* input, size_t start,
                                    size_t length);
void highlight_variable_assignment(ic_highlight_env_t* henv, const char* input,
                                   size_t absolute_start, const std::string& token);
void highlight_assignment_value(ic_highlight_env_t* henv, const char* input, size_t absolute_start,
                                const std::string& value);
void highlight_history_expansions(ic_highlight_env_t* henv, const char* input, size_t len);
struct HeredocRange {
    size_t start;
    size_t end;
    bool is_delimiter;
};
std::vector<HeredocRange> find_heredoc_ranges(const char* input, size_t len);
void highlight_compound_redirections(ic_highlight_env_t* henv, const char* input, size_t start,
                                     size_t length);

}  // namespace highlight_helpers

#endif  // CJSH_CORE_SRC_HIGHLIGHTER_HIGHLIGHT_HELPERS_H
