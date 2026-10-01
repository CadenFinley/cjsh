/*
  interpreter_utils.cpp

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

#include "interpreter_utils.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "parser_utils.h"
#include "signal_handler.h"
#include "string_utils.h"

namespace shell_script_interpreter::detail {

std::string trim(const std::string& s) {
    return string_utils::trim_ascii_whitespace_copy(s);
}

size_t find_inline_comment_start(const std::string& s, size_t start, size_t end) {
    if (start >= s.size()) {
        return std::string::npos;
    }
    end = std::min(end, s.size());

    bool in_quotes = false;
    bool in_brace_expansion = false;
    char quote = '\0';

    for (size_t i = start; i < end; ++i) {
        char c = s[i];

        if (!in_quotes && c == '$' && i + 1 < end && s[i + 1] == '{') {
            in_brace_expansion = true;
        } else if (in_brace_expansion && c == '}') {
            in_brace_expansion = false;
        }

        if (!in_quotes && !in_brace_expansion && c == '$' && i + 1 < end) {
            char next = s[i + 1];
            if (next == '#' || next == '?' || next == '$' || next == '*' || next == '@' ||
                next == '!' || (std::isdigit(static_cast<unsigned char>(next)) != 0)) {
                ++i;
                continue;
            }
        }

        if (c == '"' || c == '\'') {
            if (!is_char_escaped(s, i)) {
                if (!in_quotes) {
                    in_quotes = true;
                    quote = c;
                } else if (quote == c) {
                    in_quotes = false;
                    quote = '\0';
                }
            }
        } else if (!in_quotes && !in_brace_expansion && c == '#' && !is_char_escaped(s, i) &&
                   (i == start ||
                    (!is_char_escaped(s, i - 1) &&
                     (std::isspace(static_cast<unsigned char>(s[i - 1])) ||
                      std::string_view(";&|()<>").find(s[i - 1]) != std::string_view::npos)))) {
            return i;
        }
    }
    return std::string::npos;
}

std::string strip_inline_comment(const std::string& s) {
    if (s.find('#') == std::string::npos) {
        return s;
    }
    std::string result = s;
    size_t comment_start = find_inline_comment_start(result, 0, std::string::npos);
    while (comment_start != std::string::npos) {
        const size_t newline = result.find('\n', comment_start);
        if (newline == std::string::npos) {
            result.erase(comment_start);
            break;
        }
        // Keep offsets and later lines intact when the input contains a whole group.
        result.replace(comment_start, newline - comment_start, newline - comment_start, ' ');
        comment_start = find_inline_comment_start(result, newline + 1, std::string::npos);
    }
    return result;
}

std::string process_line_for_validation(const std::string& line) {
    return trim(strip_inline_comment(line));
}

std::vector<std::string> split_ampersand(const std::string& s) {
    if (s.find('&') == std::string::npos) {
        std::string part = trim(s);
        if (part.empty()) {
            return {};
        }
        return {std::move(part)};
    }
    std::vector<std::string> parts;
    bool in_quotes = false;
    char q = '\0';
    int arith_depth = 0;
    int bracket_depth = 0;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c == '"' || c == '\'') && (i == 0 || s[i - 1] != '\\')) {
            if (!in_quotes) {
                in_quotes = true;
                q = c;
            } else if (q == c) {
                in_quotes = false;
                q = '\0';
            }
            cur += c;
        } else if (!in_quotes) {
            if (is_char_escaped(s, i)) {
                cur += c;
                continue;
            }
            if (i >= 2 && s[i - 2] == '$' && s[i - 1] == '(' && s[i] == '(') {
                arith_depth++;
                cur += c;
            }

            else if (i + 1 < s.size() && s[i] == ')' && s[i + 1] == ')' && arith_depth > 0) {
                arith_depth--;
                cur += c;
                cur += s[i + 1];
                i++;
            }

            else if (i + 1 < s.size() && s[i] == '[' && s[i + 1] == '[') {
                bracket_depth++;
                cur += c;
                cur += s[i + 1];
                i++;
            }

            else if (i + 1 < s.size() && s[i] == ']' && s[i + 1] == ']' && bracket_depth > 0) {
                bracket_depth--;
                cur += c;
                cur += s[i + 1];
                i++;
            }

            else if (c == '&' && arith_depth == 0 && bracket_depth == 0) {
                if (i + 1 < s.size() && s[i + 1] == '&') {
                    cur += c;
                    cur += s[i + 1];
                    ++i;
                } else if (i + 1 < s.size() && s[i + 1] == '^') {
                    cur += c;
                    cur += s[i + 1];
                    if (i + 2 < s.size() && s[i + 2] == '!') {
                        cur += s[i + 2];
                        i += 2;
                    } else {
                        ++i;
                    }
                } else if (i > 0 && (s[i - 1] == '>' || s[i - 1] == '<')) {
                    // Descriptor duplication targets may be expanded (for example,
                    // >&${COPROC[1]}).  Keep the ampersand attached to the
                    // redirection and let the parser validate the expanded target.
                    cur += c;
                } else if (i + 1 < s.size() && s[i + 1] == '>') {
                    cur += c;
                } else {
                    std::string seg = trim(cur);
                    if (!seg.empty() && seg.back() != '&') {
                        seg += " &";
                    }
                    if (!seg.empty()) {
                        parts.push_back(seg);
                    }
                    cur.clear();
                }
            } else {
                cur += c;
            }
        } else {
            cur += c;
        }
    }
    std::string tail = trim(cur);
    if (!tail.empty()) {
        parts.push_back(tail);
    }
    return parts;
}

std::string to_lower_copy(std::string value) {
    return string_utils::to_lower_copy(std::move(value));
}

bool is_readable_file(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), R_OK) == 0;
}

bool is_control_flow_exit_code(int code) {
    return code == 253 || code == 254 || code == 255;
}

int pending_signal_exit_code(const SignalProcessingResult& result) {
#ifdef SIGTERM
    if (result.sigterm) {
        return 128 + SIGTERM;
    }
#endif
#ifdef SIGHUP
    if (result.sighup) {
        return 128 + SIGHUP;
    }
#endif
#ifdef SIGINT
    if (result.sigint ||
        (SignalHandler::startup_interrupted() && !SignalHandler::executing_trap())) {
        return 128 + SIGINT;
    }
#endif
    return -1;
}

bool should_skip_line(const std::string& line) {
    return line == "fi" || line == "then" || line == "else" || line == "done" || line == "esac" ||
           line == "}" || line == ";;";
}

bool contains_token(const std::string& text, const std::string& token) {
    if (text.empty()) {
        return false;
    }
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        normalized.push_back((ch == ';') ? ' ' : ch);
    }
    std::istringstream iss(normalized);
    std::string word;
    while (iss >> word) {
        if (word == token) {
            return true;
        }
    }
    return false;
}

}  // namespace shell_script_interpreter::detail
