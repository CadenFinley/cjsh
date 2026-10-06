/*
  redirection_utils.h

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

#ifndef CJSH_CORE_SRC_UTILS_REDIRECTION_UTILS_H
#define CJSH_CORE_SRC_UTILS_REDIRECTION_UTILS_H

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace redirection_utils {

enum class RedirectionOperator : unsigned char {
    Input,
    Output,
    Append,
    ForceOutput,
    BothOutput,
    HereDoc,
    HereDocStrip,
    HereString,
    ReadWrite,
    DupInput,
    DupOutput,
    StderrOutput,
    StderrAppend,
    StderrToStdout,
    StdoutToStderr
};

struct ParsedRedirectionOperator {
    RedirectionOperator op;
    size_t length;
};

std::optional<RedirectionOperator> parse_operator_token(std::string_view token);
std::optional<ParsedRedirectionOperator> parse_operator_at(std::string_view text, size_t start);
bool requires_operand(RedirectionOperator op);
const char* operator_spelling(RedirectionOperator op);
const std::vector<std::string_view>& canonical_operator_spellings();

}  // namespace redirection_utils

#endif  // CJSH_CORE_SRC_UTILS_REDIRECTION_UTILS_H
