/*
  string_utils.h

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

#ifndef CJSH_CORE_SRC_UTILS_STRING_UTILS_H
#define CJSH_CORE_SRC_UTILS_STRING_UTILS_H

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace string_utils {

inline std::vector<size_t> character_offsets(std::string_view text) {
    std::vector<size_t> offsets{0};
    std::mbstate_t state{};
    for (size_t pos = 0; pos < text.size();) {
        size_t length = std::mbrlen(text.data() + pos, text.size() - pos, &state);
        if (length == static_cast<size_t>(-1) || length == static_cast<size_t>(-2) || length == 0) {
            state = {};
            length = 1;
        }
        pos += length;
        offsets.push_back(pos);
    }
    return offsets;
}

inline std::string to_lower_copy(std::string value) {
    (void)std::transform(value.begin(), value.end(), value.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline std::string to_upper_copy(std::string value) {
    (void)std::transform(value.begin(), value.end(), value.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

inline std::string trim_left_ascii_whitespace_copy(const std::string& input) {
    const size_t begin = input.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) {
        return "";
    }
    return input.substr(begin);
}

inline std::string trim_right_ascii_whitespace_copy(const std::string& input) {
    const size_t end = input.find_last_not_of(" \t\n\r");
    if (end == std::string::npos) {
        return "";
    }
    return input.substr(0, end + 1);
}

inline std::string trim_ascii_whitespace_copy(const std::string& input) {
    const size_t begin = input.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = input.find_last_not_of(" \t\n\r");
    return input.substr(begin, end - begin + 1);
}

inline std::string trim_trailing_line_endings_copy(std::string input) {
    while (!input.empty() && (input.back() == '\n' || input.back() == '\r')) {
        input.pop_back();
    }
    return input;
}

inline bool equals_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return a == b || std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
    });
}

inline bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), value.begin(), [](char a, char b) {
        return a == b || std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
    });
}

inline std::string join_strings(const std::vector<std::string>& values, std::string_view separator,
                                size_t start_index = 0) {
    if (start_index >= values.size()) {
        return {};
    }

    size_t total_length = 0;
    for (size_t i = start_index; i < values.size(); ++i) {
        total_length += values[i].size();
        if (i + 1 < values.size()) {
            total_length += separator.size();
        }
    }

    std::string result;
    result.reserve(total_length);
    for (size_t i = start_index; i < values.size(); ++i) {
        if (i > start_index) {
            (void)result.append(separator);
        }
        (void)result.append(values[i]);
    }
    return result;
}

}  // namespace string_utils

#endif  // CJSH_CORE_SRC_UTILS_STRING_UTILS_H
