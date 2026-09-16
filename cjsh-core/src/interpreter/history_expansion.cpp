/*
  history_expansion.cpp

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

#include "history_expansion.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "cjsh_filesystem.h"
#include "command_line_utils.h"
#include "history_file_utils.h"
#include "quote_info.h"
#include "string_utils.h"

namespace {

bool begin_history_event(const std::string& command, size_t& pos, size_t& start) {
    if (pos >= command.length() || command[pos] != '!') {
        return false;
    }
    start = pos++;
    return true;
}

bool parse_number(const std::string& str, size_t& pos, int& number) {
    size_t start = pos;
    bool negative = false;

    if (pos < str.length() && str[pos] == '-') {
        negative = true;
        pos++;
    }

    if (pos >= str.length() || !std::isdigit(str[pos])) {
        pos = start;
        return false;
    }

    number = 0;
    while (pos < str.length() && std::isdigit(str[pos])) {
        number = number * 10 + (str[pos] - '0');
        pos++;
    }

    if (negative) {
        number = -number;
    }

    return true;
}

}  // namespace

bool HistoryExpansion::is_word_char(char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

std::vector<std::string> HistoryExpansion::split_into_words(const std::string& command) {
    return command_line_utils::tokenize_shell_words(command, true);
}

std::string HistoryExpansion::get_word_from_command(const std::string& command, int word_index) {
    std::vector<std::string> words = split_into_words(command);

    if (word_index < 0) {
        word_index = static_cast<int>(words.size()) + word_index;
    }

    if (word_index >= 0 && word_index < static_cast<int>(words.size())) {
        return words[static_cast<size_t>(word_index)];
    }

    return "";
}

std::string HistoryExpansion::get_words_range(const std::string& command, int start, int end) {
    std::vector<std::string> words = split_into_words(command);

    if (start < 0) {
        start = static_cast<int>(words.size()) + start;
    }
    if (end < 0) {
        end = static_cast<int>(words.size()) + end;
    }

    if (start < 0 || end < 0 || start >= static_cast<int>(words.size()) ||
        end >= static_cast<int>(words.size()) || start > end) {
        return "";
    }

    std::vector<std::string> range_words;
    range_words.reserve(static_cast<size_t>(end - start + 1));
    for (int i = start; i <= end; ++i) {
        range_words.push_back(words[static_cast<size_t>(i)]);
    }

    return string_utils::join_strings(range_words, " ");
}

bool HistoryExpansion::expand_double_bang(const std::string& command, size_t& pos,
                                          const std::vector<std::string>& history,
                                          std::string& result, std::string& error) {
    if (pos + 1 >= command.length() || command[pos] != '!' || command[pos + 1] != '!') {
        return false;
    }

    if (history.empty()) {
        error = "!!: event not found";
        return false;
    }

    const std::string& last_command = history.back();

    pos += 2;

    if (pos < command.length() && command[pos] == ':') {
        return expand_word_designator(command, pos, last_command, result, error);
    }

    result += last_command;
    return true;
}

bool HistoryExpansion::expand_history_number(const std::string& command, size_t& pos,
                                             const std::vector<std::string>& history,
                                             std::string& result, std::string& error) {
    size_t start = 0;
    if (!begin_history_event(command, pos, start)) {
        return false;
    }

    int number;
    if (!parse_number(command, pos, number)) {
        pos = start;
        return false;
    }

    std::string referenced_command;
    if (number < 0) {
        int index = static_cast<int>(history.size()) + number;
        if (index < 0 || index >= static_cast<int>(history.size())) {
            error = "!" + std::to_string(number) + ": event not found";
            return false;
        }
        referenced_command = history[static_cast<size_t>(index)];
    } else {
        if (number >= static_cast<int>(history.size())) {
            error = "!" + std::to_string(number) + ": event not found";
            return false;
        }
        referenced_command = history[static_cast<size_t>(number)];
    }

    if (pos < command.length() && command[pos] == ':') {
        return expand_word_designator(command, pos, referenced_command, result, error);
    }

    result += referenced_command;
    return true;
}

bool HistoryExpansion::expand_history_search(const std::string& command, size_t& pos,
                                             const std::vector<std::string>& history,
                                             std::string& result, std::string& error) {
    size_t start = 0;
    if (!begin_history_event(command, pos, start)) {
        return false;
    }

    bool search_substring = false;
    if (pos < command.length() && command[pos] == '?') {
        search_substring = true;
        pos++;
    }

    std::string pattern;
    while (pos < command.length() && is_word_char(command[pos])) {
        pattern += command[pos];
        pos++;
    }

    if (search_substring && pos < command.length() && command[pos] == '?') {
        pos++;
    }

    if (pattern.empty()) {
        pos = start;
        return false;
    }

    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        bool matches = false;
        if (search_substring) {
            matches = it->find(pattern) != std::string::npos;
        } else {
            matches = it->find(pattern) == 0;
        }

        if (matches) {
            if (pos < command.length() && command[pos] == ':') {
                return expand_word_designator(command, pos, *it, result, error);
            }
            result += *it;
            return true;
        }
    }

    error = "!" + pattern + ": event not found";
    return false;
}

bool HistoryExpansion::expand_previous_command_designator(const std::string& command, size_t& pos,
                                                          const std::vector<std::string>& history,
                                                          std::string& result, std::string& error) {
    if (pos + 1 >= command.length() || command[pos] != '!') {
        return false;
    }

    const char next = command[pos + 1];
    if (next != ':' && next != '^' && next != '$' && next != '*') {
        return false;
    }

    if (history.empty()) {
        error =
            (next == ':') ? "!: event not found" : std::string("!") + next + ": event not found";
        return false;
    }

    const std::string& last_command = history.back();
    if (next == ':') {
        pos++;
        return expand_word_designator(command, pos, last_command, result, error);
    }

    pos += 2;

    if (next == '^') {
        std::string word = get_word_from_command(last_command, 1);
        if (word.empty()) {
            error = "!^: bad word specifier";
            return false;
        }
        result += word;
        return true;
    }

    if (next == '$') {
        std::string word = get_word_from_command(last_command, -1);
        if (word.empty()) {
            error = "!$: bad word specifier";
            return false;
        }
        result += word;
        return true;
    }

    std::string words = get_words_range(last_command, 1, -1);
    if (words.empty()) {
        error = "!*: bad word specifier";
        return false;
    }
    result += words;
    return true;
}

bool HistoryExpansion::expand_quick_substitution(const std::string& command,
                                                 const std::vector<std::string>& history,
                                                 std::string& result, std::string& error) {
    if (command.empty() || command[0] != '^') {
        return false;
    }

    size_t second_caret = command.find('^', 1);
    if (second_caret == std::string::npos) {
        return false;
    }

    std::string old_text = command.substr(1, second_caret - 1);
    if (old_text.empty()) {
        error = "quick substitution: empty search string";
        return false;
    }

    size_t third_caret = command.find('^', second_caret + 1);
    std::string new_text;
    if (third_caret != std::string::npos) {
        new_text = command.substr(second_caret + 1, third_caret - second_caret - 1);
    } else {
        new_text = command.substr(second_caret + 1);
    }

    if (history.empty()) {
        error = "^" + old_text + "^" + new_text + ": event not found";
        return false;
    }

    const std::string& last_command = history.back();
    size_t pos = last_command.find(old_text);
    if (pos == std::string::npos) {
        error = "^" + old_text + "^" + new_text + ": substitution failed";
        return false;
    }

    result = last_command.substr(0, pos) + new_text + last_command.substr(pos + old_text.length());
    if (third_caret != std::string::npos) {
        result += command.substr(third_caret + 1);
    }
    return true;
}

bool HistoryExpansion::expand_word_designator(const std::string& command, size_t& pos,
                                              const std::string& referenced_command,
                                              std::string& result, std::string& error) {
    if (pos >= command.length() || command[pos] != ':') {
        result += referenced_command;
        return true;
    }

    pos++;

    if (pos >= command.length()) {
        result += referenced_command;
        return true;
    }

    char designator = command[pos];
    pos++;

    if (designator == '^') {
        std::string word = get_word_from_command(referenced_command, 1);
        if (word.empty()) {
            error = "!:^: bad word specifier";
            return false;
        }
        result += word;
    } else if (designator == '$') {
        std::string word = get_word_from_command(referenced_command, -1);
        if (word.empty()) {
            error = "!:$: bad word specifier";
            return false;
        }
        result += word;
    } else if (designator == '*') {
        std::vector<std::string> words = split_into_words(referenced_command);
        if (words.size() <= 1) {
            error = "!:*: bad word specifier";
            return false;
        }
        for (size_t i = 1; i < words.size(); ++i) {
            if (i > 1) {
                result += ' ';
            }
            result += words[i];
        }
    } else if (std::isdigit(designator)) {
        pos--;
        int start_index;
        if (!parse_number(command, pos, start_index)) {
            error = "bad word specifier";
            return false;
        }

        if (pos < command.length() && command[pos] == '-') {
            pos++;
            int end_index;
            if (pos < command.length() && std::isdigit(command[pos])) {
                if (!parse_number(command, pos, end_index)) {
                    error = "bad word specifier";
                    return false;
                }
            } else {
                end_index = -1;
            }

            std::string words = get_words_range(referenced_command, start_index, end_index);
            if (words.empty()) {
                error = "bad word specifier";
                return false;
            }
            result += words;
        } else {
            std::string word = get_word_from_command(referenced_command, start_index);
            if (word.empty()) {
                error = "bad word specifier";
                return false;
            }
            result += word;
        }
    } else {
        result += referenced_command;
    }

    return true;
}

bool HistoryExpansion::contains_history_expansion(const std::string& command) {
    if (command.empty()) {
        return false;
    }

    if (command[0] == '^') {
        size_t second_caret = command.find('^', 1);
        if (second_caret != std::string::npos) {
            return true;
        }
    }

    for (size_t i = 0; i < command.length(); ++i) {
        if (command[i] == '!' && !is_inside_quotes(command, i)) {
            if (i > 0 && command[i - 1] == '\\') {
                continue;
            }

            if (i + 1 < command.length()) {
                char next = command[i + 1];

                if (next == '!' || next == '$' || next == '^' || next == '*' || next == ':' ||
                    std::isdigit(next) || next == '-' || std::isalpha(next) || next == '?') {
                    return true;
                }
            }
        }
    }

    return false;
}

HistoryExpansion::ExpansionResult HistoryExpansion::expand(
    const std::string& command, const std::vector<std::string>& history_entries,
    bool history_includes_current_command) {
    ExpansionResult expansion_result;
    expansion_result.expanded_command = command;
    expansion_result.was_expanded = false;
    expansion_result.should_echo = true;

    if (command.empty() || !contains_history_expansion(command)) {
        expansion_result.should_echo = false;
        return expansion_result;
    }

    const std::vector<std::string>* expansion_history = &history_entries;
    std::vector<std::string> committed_history;
    if (history_includes_current_command && !history_entries.empty()) {
        // Interactive submit stages the current buffer as the newest history entry.
        committed_history = history_entries;
        committed_history.pop_back();
        expansion_history = &committed_history;
    }

    if (command[0] == '^') {
        std::string result;
        std::string error;
        if (expand_quick_substitution(command, *expansion_history, result, error)) {
            expansion_result.expanded_command = result;
            expansion_result.was_expanded = true;
            return expansion_result;
        } else {
            expansion_result.has_error = true;
            expansion_result.error_message = error;
            return expansion_result;
        }
    }

    std::string result;
    result.reserve(command.length() * 2);

    for (size_t i = 0; i < command.length(); ++i) {
        if (is_inside_quotes(command, i)) {
            result += command[i];
            continue;
        }

        if (i > 0 && command[i - 1] == '\\' && command[i] == '!') {
            result += command[i];
            continue;
        }

        if (command[i] == '!') {
            std::string error;
            bool expanded = false;

            size_t saved_pos = i;

            if (!expanded && i + 1 < command.length() && command[i + 1] == '!') {
                expanded = expand_double_bang(command, i, *expansion_history, result, error);
            }

            if (!expanded && i + 1 < command.length() &&
                (command[i + 1] == ':' || command[i + 1] == '$' || command[i + 1] == '^' ||
                 command[i + 1] == '*')) {
                i = saved_pos;
                expanded = expand_previous_command_designator(command, i, *expansion_history,
                                                              result, error);
            }

            if (!expanded && i + 1 < command.length() &&
                (std::isdigit(command[i + 1]) || command[i + 1] == '-')) {
                i = saved_pos;
                expanded = expand_history_number(command, i, *expansion_history, result, error);
            }

            if (!expanded && i + 1 < command.length() &&
                (std::isalpha(command[i + 1]) || command[i + 1] == '?')) {
                i = saved_pos;
                expanded = expand_history_search(command, i, *expansion_history, result, error);
            }

            if (expanded) {
                expansion_result.was_expanded = true;
                i--;
            } else if (!error.empty()) {
                expansion_result.has_error = true;
                expansion_result.error_message = error;
                return expansion_result;
            } else {
                result += command[i];
            }
        } else {
            result += command[i];
        }
    }

    expansion_result.expanded_command = result;
    return expansion_result;
}

std::string HistoryExpansion::get_history_file_path() {
    cjsh_filesystem::initialize_history_storage();
    return cjsh_filesystem::g_cjsh_history_path().string();
}

std::vector<std::string> HistoryExpansion::read_history_entries() {
    std::string history_path = get_history_file_path();
    return history_file_utils::read_history_entries(history_path);
}
