/*
  quote_info.h

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

#ifndef CJSH_CORE_SRC_PARSER_QUOTE_INFO_H
#define CJSH_CORE_SRC_PARSER_QUOTE_INFO_H

#include <string>
#include <vector>

extern const char QUOTE_PREFIX;
extern const char QUOTE_SINGLE;
extern const char QUOTE_DOUBLE;

std::string create_quote_tag(char quote_type, const std::string& content);

// Remove per-character escape markers once their protection is no longer needed.
std::string remove_escape_markers(const std::string& value);

bool is_inside_quotes(const std::string& text, size_t pos);

struct QuoteInfo {
    bool is_single;
    bool is_double;
    std::string value;

    QuoteInfo(const std::string& token);

    bool is_unquoted() const;
    std::string unescaped_value() const;

   private:
    static bool is_single_quoted_token(const std::string& s);
    static bool is_double_quoted_token(const std::string& s);
    static std::string strip_quote_tag(const std::string& s);
};

std::vector<std::string> expand_tilde_tokens(const std::vector<std::string>& tokens);

#endif  // CJSH_CORE_SRC_PARSER_QUOTE_INFO_H
