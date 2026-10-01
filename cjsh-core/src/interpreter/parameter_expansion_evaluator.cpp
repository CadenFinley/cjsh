/*
  parameter_expansion_evaluator.cpp

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

#include "parameter_expansion_evaluator.h"
#include "parser_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
bool is_literal_pattern(const std::string& pattern) {
    // Quoted, escaped and glob patterns still need the shell pattern matcher.
    return pattern.find_first_of("*?[\\\"'()|") == std::string::npos;
}
}  // namespace

ParameterExpansionEvaluator::ParameterExpansionEvaluator(
    VariableReader var_reader, VariableWriter var_writer, VariableChecker var_checker,
    PatternMatcher pattern_matcher, ArrayLengthReader array_length_reader,
    ArrayKeysReader array_keys_reader, WordExpander word_expander, IndirectReader indirect_reader,
    PatternEndpoints pattern_endpoints, WordExpander pattern_word_expander)
    : read_variable(std::move(var_reader)),
      write_variable(std::move(var_writer)),
      is_variable_set(std::move(var_checker)),
      matches_pattern(std::move(pattern_matcher)),
      read_array_length(std::move(array_length_reader)),
      read_array_keys(std::move(array_keys_reader)),
      expand_word(std::move(word_expander)),
      expand_pattern_word(std::move(pattern_word_expander)),
      read_indirect(std::move(indirect_reader)),
      find_pattern_endpoints(std::move(pattern_endpoints)) {
}

std::string ParameterExpansionEvaluator::expand(const std::string& param_expr) {
    if (param_expr.empty()) {
        return "";
    }

    if (config::is_posix_mode()) {
        const size_t end = posix_parameter_name_end(param_expr);
        if (end == std::string::npos) {
            throw std::runtime_error("parameter expansion error: ${" + param_expr +
                                     "} is disabled in POSIX mode");
        }
        const bool length = param_expr[0] == '#' && param_expr.size() > 1;
        const std::string name = param_expr.substr(length ? 1 : 0, end - (length ? 1 : 0));
        if (end == param_expr.size() && name != "@" && name != "*" && g_shell &&
            g_shell->get_shell_option(ShellOption::Nounset) && !is_variable_set(name)) {
            throw std::runtime_error(name + ": parameter not set");
        }
        if (param_expr.size() == 1) {
            return read_variable(param_expr);
        }
    }

    if (param_expr[0] == '!') {
        if (read_array_keys) {
            std::string array_expr = param_expr.substr(1);
            size_t lb = array_expr.find('[');
            if (lb != std::string::npos && !array_expr.empty() && array_expr.back() == ']') {
                std::string index = array_expr.substr(lb + 1, array_expr.length() - lb - 2);
                if (index == "@" || index == "*") {
                    return read_array_keys(array_expr);
                }
            }
        }

        std::string var_name = param_expr.substr(1);
        if (read_indirect) {
            return read_indirect(var_name);
        }
        std::string indirect_name = read_variable(var_name);
        return read_variable(indirect_name);
    }

    if (param_expr[0] == '#') {
        std::string var_name = param_expr.substr(1);
        if (read_array_length) {
            std::optional<size_t> array_length = read_array_length(var_name);
            if (array_length.has_value()) {
                return std::to_string(*array_length);
            }
        }
        std::string value = read_variable(var_name);
        return std::to_string(string_utils::character_offsets(value).size() - 1);
    }

    std::string substring_result;
    if (try_evaluate_substring(param_expr, substring_result)) {
        return substring_result;
    }

    size_t op_pos = std::string::npos;
    std::string op;

    auto is_operator_start = [](char c) {
        switch (c) {
            case ':':
            case '#':
            case '%':
            case '/':
            case '^':
            case ',':
            case '-':
            case '=':
            case '?':
            case '+':
                return true;
            default:
                return false;
        }
    };

    for (size_t i = 1; i < param_expr.length(); ++i) {
        if (is_operator_start(param_expr[i])) {
            op_pos = i;
            break;
        }
    }

    if (op_pos != std::string::npos) {
        char op_char = param_expr[op_pos];
        switch (op_char) {
            case ':': {
                if (op_pos + 1 < param_expr.length()) {
                    char next = param_expr[op_pos + 1];
                    if (next == '-' || next == '=' || next == '?' || next == '+') {
                        op = param_expr.substr(op_pos, 2);
                    }
                }
                break;
            }
            case '#': {
                if (op_pos + 1 < param_expr.length() && param_expr[op_pos + 1] == '#') {
                    op = "##";
                } else {
                    op = "#";
                }
                break;
            }
            case '%': {
                if (op_pos + 1 < param_expr.length() && param_expr[op_pos + 1] == '%') {
                    op = "%%";
                } else {
                    op = "%";
                }
                break;
            }
            case '/': {
                if (op_pos + 1 < param_expr.length() && param_expr[op_pos + 1] == '/') {
                    op = "//";
                } else {
                    op = "/";
                }
                break;
            }
            case '^': {
                if (op_pos + 1 < param_expr.length() && param_expr[op_pos + 1] == '^') {
                    op = "^^";
                } else {
                    op = "^";
                }
                break;
            }
            case ',': {
                if (op_pos + 1 < param_expr.length() && param_expr[op_pos + 1] == ',') {
                    op = ",,";
                } else {
                    op = ",";
                }
                break;
            }
            case '-':
            case '=':
            case '?':
            case '+': {
                op = param_expr.substr(op_pos, 1);
                break;
            }
            default:
                break;
        }
    }

    if (op.empty()) {
        op_pos = std::string::npos;
    }

    std::string var_name = param_expr.substr(0, op_pos);
    std::string var_value = read_variable(var_name);

    if (op_pos == std::string::npos) {
        if (config::is_posix_mode() && g_shell && g_shell->get_shell_option(ShellOption::Nounset) &&
            var_name != "@" && var_name != "*" && !is_variable_set(var_name)) {
            throw std::runtime_error("parameter expansion error: " + var_name +
                                     ": parameter not set");
        }
        return var_value;
    }

    const bool needs_presence = op.find_first_of("-=+?") != std::string::npos;
    const bool is_set = needs_presence && is_variable_set(var_name);

    std::string operand = param_expr.substr(op_pos + op.length());
    auto expand_operand = [&](bool pattern = false) -> std::string {
        if (pattern && expand_pattern_word) {
            return expand_pattern_word(operand);
        }
        return expand_word ? expand_word(operand) : operand;
    };

    if (op == ":-") {
        return (is_set && !var_value.empty()) ? var_value : expand_operand();
    }
    if (op == "-") {
        return is_set ? var_value : expand_operand();
    }

    if (op == ":=") {
        if (!is_set || var_value.empty()) {
            std::string expanded_operand = expand_operand();
            write_variable(var_name, expanded_operand);
            return expanded_operand;
        }
        return var_value;
    }
    if (op == "=") {
        if (!is_set) {
            std::string expanded_operand = expand_operand();
            write_variable(var_name, expanded_operand);
            return expanded_operand;
        }
        return var_value;
    }

    if (op == ":?") {
        if (!is_set || var_value.empty()) {
            std::string expanded_operand = operand.empty() ? operand : expand_operand();
            std::string error_msg =
                var_name + ": " +
                (expanded_operand.empty() ? "parameter null or not set" : expanded_operand);
            throw std::runtime_error("parameter expansion error: " + error_msg + " in ${" +
                                     var_name + op + operand + "}");
        }
        return var_value;
    }
    if (op == "?") {
        if (!is_set) {
            std::string expanded_operand = operand.empty() ? operand : expand_operand();
            std::string error_msg =
                var_name + ": " +
                (expanded_operand.empty() ? "parameter not set" : expanded_operand);
            throw std::runtime_error("parameter expansion error: " + error_msg + " in ${" +
                                     var_name + op + operand + "}");
        }
        return var_value;
    }

    if (op == ":+") {
        return (is_set && !var_value.empty()) ? expand_operand() : "";
    }
    if (op == "+") {
        return is_set ? expand_operand() : "";
    }

    if (op == "#") {
        return pattern_match_prefix(var_value, expand_operand(true), false);
    }
    if (op == "##") {
        return pattern_match_prefix(var_value, expand_operand(true), true);
    }

    if (op == "%") {
        return pattern_match_suffix(var_value, expand_operand(true), false);
    }
    if (op == "%%") {
        return pattern_match_suffix(var_value, expand_operand(true), true);
    }

    if (op == "/") {
        return pattern_substitute(var_value, operand, false);
    }
    if (op == "//") {
        return pattern_substitute(var_value, operand, true);
    }

    if (op == "^") {
        return case_convert(var_value, operand, true, false);
    }
    if (op == "^^") {
        return case_convert(var_value, operand, true, true);
    }
    if (op == ",") {
        return case_convert(var_value, operand, false, false);
    }
    if (op == ",,") {
        return case_convert(var_value, operand, false, true);
    }

    return var_value;
}

std::string ParameterExpansionEvaluator::pattern_match_prefix(const std::string& value,
                                                              const std::string& pattern,
                                                              bool longest) {
    if (value.empty() || pattern.empty()) {
        return value;
    }

    // A literal has only one possible match length, for both # and ##.
    if (is_literal_pattern(pattern)) {
        return value.compare(0, pattern.size(), pattern) == 0 ? value.substr(pattern.size())
                                                              : value;
    }

    if (find_pattern_endpoints) {
        if ((longest && matches_pattern(value, pattern)) ||
            (!longest && matches_pattern({}, pattern))) {
            return longest ? std::string{} : value;
        }
        if (auto endpoints = find_pattern_endpoints(value, pattern, longest)) {
            return (*endpoints)[0] == std::string::npos ? value : value.substr((*endpoints)[0]);
        }
    }

    const auto offsets = string_utils::character_offsets(value);
    for (size_t step = 0; step < offsets.size(); ++step) {
        const size_t i = offsets[longest ? offsets.size() - step - 1 : step];
        std::string prefix = value.substr(0, i);
        if (matches_pattern(prefix, pattern)) {
            return value.substr(i);
        }
    }

    return value;
}

std::string ParameterExpansionEvaluator::pattern_match_suffix(const std::string& value,
                                                              const std::string& pattern,
                                                              bool longest) {
    if (value.empty() || pattern.empty()) {
        return value;
    }

    if (is_literal_pattern(pattern)) {
        if (pattern.size() <= value.size() &&
            value.compare(value.size() - pattern.size(), pattern.size(), pattern) == 0) {
            return value.substr(0, value.size() - pattern.size());
        }
        return value;
    }

    if (find_pattern_endpoints) {
        if ((longest && matches_pattern(value, pattern)) ||
            (!longest && matches_pattern({}, pattern))) {
            return longest ? std::string{} : value;
        }
        // A suffix must reach the end of the value, regardless of which matching
        // suffix length the caller requests.
        if (auto endpoints = find_pattern_endpoints(value, pattern, true)) {
            for (size_t step = 0; step <= value.size(); ++step) {
                const size_t begin = longest ? step : value.size() - step;
                if ((*endpoints)[begin] == value.size()) {
                    return value.substr(0, begin);
                }
            }
            return value;
        }
    }

    const auto offsets = string_utils::character_offsets(value);
    for (size_t step = 0; step < offsets.size(); ++step) {
        const size_t begin = offsets[longest ? step : offsets.size() - step - 1];
        std::string suffix = value.substr(begin);
        if (matches_pattern(suffix, pattern)) {
            return value.substr(0, begin);
        }
    }

    return value;
}

std::string ParameterExpansionEvaluator::pattern_substitute(const std::string& value,
                                                            const std::string& replacement_expr,
                                                            bool global) {
    if (value.empty() || replacement_expr.empty()) {
        return value;
    }

    size_t slash_pos = std::string::npos;
    bool escaped = false;
    for (size_t i = 0; i < replacement_expr.size(); ++i) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (replacement_expr[i] == '\\') {
            escaped = true;
            continue;
        }
        if (replacement_expr[i] == '/') {
            slash_pos = i;
            break;
        }
    }
    if (slash_pos == std::string::npos) {
        return value;
    }

    std::string pattern = replacement_expr.substr(0, slash_pos);
    std::string replacement = replacement_expr.substr(slash_pos + 1);

    auto unescape_slashes = [](std::string text) {
        size_t position = 0;
        while ((position = text.find("\\/", position)) != std::string::npos) {
            (void)text.erase(position, 1);
            ++position;
        }
        return text;
    };
    pattern = unescape_slashes(std::move(pattern));
    replacement = unescape_slashes(std::move(replacement));

    if (pattern.empty()) {
        return value;
    }

    bool anchor_prefix = false;
    bool anchor_suffix = false;
    if (!global && !pattern.empty() && (pattern[0] == '#' || pattern[0] == '%')) {
        anchor_prefix = pattern[0] == '#';
        anchor_suffix = pattern[0] == '%';
        (void)pattern.erase(0, 1);
        if (pattern.empty()) {
            return value;
        }
    }

    if (anchor_prefix) {
        std::string remainder = pattern_match_prefix(value, pattern, true);
        if (remainder.length() != value.length()) {
            return replacement + remainder;
        }
        return value;
    }

    if (anchor_suffix) {
        std::string prefix = pattern_match_suffix(value, pattern, true);
        if (prefix.length() != value.length()) {
            return prefix + replacement;
        }
        return value;
    }

    struct MatchSpan {
        size_t begin;
        size_t end;
    };

    // Quoted, escaped and glob patterns still use the matcher. A plain literal
    // has only one possible match length, so there is no need to try substrings.
    const bool literal_pattern = is_literal_pattern(pattern);
    std::optional<std::vector<size_t>> endpoints;
    if (!literal_pattern && find_pattern_endpoints) {
        // Preserve the cheap common case where the first candidate consumes the
        // whole value. Otherwise reuse endpoints across every replacement.
        if (matches_pattern(value, pattern)) {
            return replacement;
        }
        endpoints = find_pattern_endpoints(value, pattern, true);
    }
    auto find_leftmost_longest = [&](size_t search_begin) -> std::optional<MatchSpan> {
        if (literal_pattern) {
            const size_t begin = value.find(pattern, search_begin);
            if (begin == std::string::npos) {
                return std::nullopt;
            }
            return MatchSpan{begin, begin + pattern.size()};
        }
        if (endpoints) {
            for (size_t begin = search_begin; begin <= value.size(); ++begin) {
                if ((*endpoints)[begin] != std::string::npos) {
                    return MatchSpan{begin, (*endpoints)[begin]};
                }
            }
            return std::nullopt;
        }
        for (size_t begin = search_begin; begin <= value.size(); ++begin) {
            for (size_t end = value.size(); end >= begin; --end) {
                if (matches_pattern(value.substr(begin, end - begin), pattern)) {
                    return MatchSpan{begin, end};
                }
                if (end == begin) {
                    break;
                }
            }
        }
        return std::nullopt;
    };

    std::string result;
    result.reserve(value.size() + replacement.size());
    size_t cursor = 0;

    while (cursor <= value.size()) {
        auto match = find_leftmost_longest(cursor);
        if (!match.has_value()) {
            result += value.substr(cursor);
            break;
        }

        result += value.substr(cursor, match->begin - cursor);
        result += replacement;

        if (!global) {
            result += value.substr(match->end);
            break;
        }

        if (match->end == match->begin) {
            if (match->end >= value.size()) {
                break;
            }
            result.push_back(value[match->end]);
            cursor = match->end + 1;
        } else {
            cursor = match->end;
            if (cursor >= value.size()) {
                break;
            }
        }
    }

    return result;
}

bool ParameterExpansionEvaluator::try_evaluate_substring(const std::string& param_expr,
                                                         std::string& result) {
    size_t colon_pos = param_expr.find(':');
    if (colon_pos == std::string::npos || colon_pos + 1 >= param_expr.length()) {
        return false;
    }

    if (colon_pos + 2 <= param_expr.length()) {
        std::string possible_op = param_expr.substr(colon_pos, 2);
        if (possible_op == ":-" || possible_op == ":=" || possible_op == ":?" ||
            possible_op == ":+") {
            return false;
        }
    }

    auto is_digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };

    size_t pos = colon_pos + 1;
    while (pos < param_expr.length() && std::isspace(static_cast<unsigned char>(param_expr[pos]))) {
        pos++;
    }

    if (pos >= param_expr.length()) {
        return false;
    }

    char marker = param_expr[pos];
    if (!is_digit(marker) && ((marker != '+' && marker != '-') || pos + 1 >= param_expr.length() ||
                              !is_digit(param_expr[pos + 1]))) {
        return false;
    }

    std::string var_name = param_expr.substr(0, colon_pos);
    std::string var_value = read_variable(var_name);

    int offset_sign = 1;
    if (pos < param_expr.length() && (param_expr[pos] == '+' || param_expr[pos] == '-')) {
        offset_sign = (param_expr[pos] == '-') ? -1 : 1;
        pos++;
    }

    const char* start_ptr = param_expr.c_str() + pos;
    char* endptr_raw = nullptr;
    long offset_value = std::strtol(start_ptr, &endptr_raw, 10);
    const char* endptr = endptr_raw;
    if (start_ptr == endptr) {
        offset_value = 0;
        endptr = start_ptr;
    }
    size_t consumed = static_cast<size_t>(endptr - param_expr.c_str());
    pos = consumed;
    offset_value *= offset_sign;

    while (pos < param_expr.length() && std::isspace(static_cast<unsigned char>(param_expr[pos]))) {
        pos++;
    }

    bool length_specified = false;
    long length_value = 0;
    if (pos < param_expr.length() && param_expr[pos] == ':') {
        length_specified = true;
        pos++;
        while (pos < param_expr.length() &&
               std::isspace(static_cast<unsigned char>(param_expr[pos]))) {
            pos++;
        }

        int length_sign = 1;
        if (pos < param_expr.length() && (param_expr[pos] == '+' || param_expr[pos] == '-')) {
            length_sign = (param_expr[pos] == '-') ? -1 : 1;
            pos++;
        }

        const char* length_ptr = param_expr.c_str() + pos;
        char* length_endptr = nullptr;
        length_value = std::strtol(length_ptr, &length_endptr, 10);
        if (length_ptr == length_endptr) {
            length_value = 0;
        }
        length_value *= length_sign;
    }

    long value_len = static_cast<long>(var_value.length());
    long start_index = offset_value;
    if (start_index < 0) {
        start_index = value_len + start_index;
    }

    if (start_index < 0) {
        start_index = 0;
    }
    if (start_index > value_len) {
        result = "";
        return true;
    }

    long slice_length;
    if (length_specified) {
        if (length_value <= 0) {
            result = "";
            return true;
        }
        slice_length = length_value;
    } else {
        slice_length = value_len - start_index;
    }

    if (start_index + slice_length > value_len) {
        slice_length = value_len - start_index;
    }

    result = var_value.substr(static_cast<size_t>(start_index), static_cast<size_t>(slice_length));
    return true;
}

std::string ParameterExpansionEvaluator::case_convert(const std::string& value,
                                                      const std::string& pattern, bool uppercase,
                                                      bool all_chars) {
    if (value.empty()) {
        return value;
    }

    std::string result = value;
    static_cast<void>(pattern);
    const auto convert = [uppercase](char c) {
        unsigned char value = static_cast<unsigned char>(c);
        return static_cast<char>(uppercase ? std::toupper(value) : std::tolower(value));
    };

    if (all_chars) {
        for (char& c : result) {
            c = convert(c);
        }
    } else {
        result[0] = convert(result[0]);
    }

    return result;
}
