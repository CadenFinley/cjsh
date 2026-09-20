/*
  quote_info.cpp

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

#include "quote_info.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cjsh_filesystem.h"
#include "quote_state.h"
#include "shell_env.h"

const char QUOTE_PREFIX = '\x1F';
const char QUOTE_SINGLE = 'S';
const char QUOTE_DOUBLE = 'D';

std::string create_quote_tag(char quote_type, const std::string& content) {
    std::string result;
    result.reserve(content.size() + 2);
    result += QUOTE_PREFIX;
    result += quote_type;
    result += content;
    return result;
}

std::string remove_escape_markers(const std::string& value) {
    if (value.find(QUOTE_PREFIX) == std::string::npos) {
        return value;
    }

    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == QUOTE_PREFIX) {
            if (++i == value.size()) {
                break;
            }
        }
        result += value[i];
    }
    return result;
}

bool is_inside_quotes(const std::string& text, size_t pos) {
    return utils::is_inside_quotes_at(text, pos);
}

QuoteInfo::QuoteInfo(const std::string& token)
    : is_single(is_single_quoted_token(token)),
      is_double(is_double_quoted_token(token)),
      value(strip_quote_tag(token)) {
}

bool QuoteInfo::is_unquoted() const {
    return !is_single && !is_double;
}

std::string QuoteInfo::unescaped_value() const {
    return is_unquoted() ? remove_escape_markers(value) : value;
}

bool QuoteInfo::is_single_quoted_token(const std::string& s) {
    return s.size() >= 2 && s[0] == '\x1F' && s[1] == 'S';
}

bool QuoteInfo::is_double_quoted_token(const std::string& s) {
    return s.size() >= 2 && s[0] == '\x1F' && s[1] == 'D';
}

std::string QuoteInfo::strip_quote_tag(const std::string& s) {
    if (s.size() >= 2 && s[0] == '\x1F' && (s[1] == 'S' || s[1] == 'D')) {
        return s.substr(2);
    }
    return s;
}

std::vector<std::string> expand_tilde_tokens(const std::vector<std::string>& tokens) {
    std::vector<std::string> result;
    result.reserve(tokens.size());

    if (config::posix_mode) {
        (void)result.insert(result.end(), tokens.begin(), tokens.end());
        return result;
    }

    std::string cwd;
    bool cwd_resolved = false;

    auto ensure_cwd = [&]() -> const std::string& {
        if (!cwd_resolved) {
            cwd = cjsh_filesystem::safe_current_directory();
            cwd_resolved = true;
        }
        return cwd;
    };

    for (const auto& raw : tokens) {
        QuoteInfo qi(raw);

        if (qi.is_unquoted() && !qi.value.empty() && qi.value.front() == '~') {
            std::filesystem::path expanded =
                cjsh_filesystem::expand_shell_path_token(qi.value, ensure_cwd(), std::string{});
            if (!expanded.empty()) {
                result.push_back(expanded.string());
            } else {
                result.push_back(qi.value);
            }
        } else if (qi.is_unquoted()) {
            result.push_back(qi.value);
        } else if (qi.is_single) {
            result.push_back(create_quote_tag(QUOTE_SINGLE, qi.value));
        } else {
            result.push_back(create_quote_tag(QUOTE_DOUBLE, qi.value));
        }
    }

    return result;
}
