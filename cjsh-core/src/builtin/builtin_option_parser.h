/*
  builtin_option_parser.h

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

#ifndef CJSH_CORE_SRC_BUILTIN_BUILTIN_OPTION_PARSER_H
#define CJSH_CORE_SRC_BUILTIN_BUILTIN_OPTION_PARSER_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct BuiltinShortOptionSpec {
    char option;
    bool requires_value;
};

struct BuiltinParsedShortOption {
    char option;
    std::optional<std::string> value;
};

bool builtin_parse_short_options_ex(const std::vector<std::string>& args, size_t& start_index,
                                    const std::string& command_name,
                                    const std::function<bool(char)>& is_valid_option,
                                    const std::function<bool(char)>& option_requires_value,
                                    std::vector<BuiltinParsedShortOption>& parsed_options,
                                    bool require_option_character = true,
                                    bool passthrough_long_options = false);

bool builtin_parse_short_options(const std::vector<std::string>& args, size_t& start_index,
                                 const std::string& command_name,
                                 const std::function<bool(char)>& handle_option,
                                 bool require_option_character = true);

#endif  // CJSH_CORE_SRC_BUILTIN_BUILTIN_OPTION_PARSER_H
