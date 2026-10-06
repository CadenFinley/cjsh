/*
  token_classifier.h

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

#ifndef CJSH_CORE_SRC_HIGHLIGHTER_TOKEN_CLASSIFIER_H
#define CJSH_CORE_SRC_HIGHLIGHTER_TOKEN_CLASSIFIER_H

#include <cstddef>
#include <string>

namespace token_classifier {

bool is_external_command(const std::string& token);
bool is_shell_keyword(const std::string& token);
bool is_shell_builtin(const std::string& token);
bool is_variable_reference(const std::string& token);
bool is_quoted_string(const std::string& token, char& quote_type);
bool is_redirection_operator(const std::string& token);
bool is_glob_pattern(const std::string& token);
bool is_option(const std::string& token);
bool is_numeric_literal(const std::string& token);
bool is_function_definition(const std::string& input, size_t& func_name_start,
                            size_t& func_name_end);

}  // namespace token_classifier

#endif  // CJSH_CORE_SRC_HIGHLIGHTER_TOKEN_CLASSIFIER_H
