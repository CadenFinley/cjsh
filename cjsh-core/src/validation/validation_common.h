/*
  validation_common.h

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

#ifndef CJSH_CORE_SRC_VALIDATION_VALIDATION_COMMON_H
#define CJSH_CORE_SRC_VALIDATION_VALIDATION_COMMON_H

#include "interpreter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace shell_validation::internal {

using SyntaxError = ShellScriptInterpreter::SyntaxError;

struct QuoteState {
    bool in_quotes = false;
    char quote_char = '\0';
    bool escaped = false;
};

enum class IterationAction : std::uint8_t {
    Continue,
    Break
};

std::string sanitize_command_substitutions_for_validation(const std::string& input);
std::vector<std::string> sanitize_lines_for_validation(const std::vector<std::string>& lines);

bool starts_with_keyword_token(const std::string& line, const std::string& keyword);
std::string extract_identifier_from_token(const std::string& token);
bool is_keyword_token(const std::string& token, const std::string& keyword);
size_t find_matching_backtick_for_validation(const std::string& text, size_t start_index);
bool find_matching_command_substitution_end_for_validation(const std::string& text,
                                                           size_t start_index, size_t& end_out);
bool is_do_token(const std::string& token);
bool is_done_token(const std::string& token);
std::string get_last_non_comment_token(const std::vector<std::string>& tokens);

bool should_process_char(QuoteState& state, char c, bool ignore_single_quotes,
                         bool process_escaped_chars = true, bool ignore_double_quotes = false);

template <typename Callback>
void for_each_effective_char(const std::string& line, bool ignore_single_quotes,
                             bool process_escaped_chars, Callback&& callback,
                             bool ignore_double_quotes = false) {
    QuoteState state;
    size_t index = 0;
    while (index < line.size()) {
        char c = line[index];
        if (!should_process_char(state, c, ignore_single_quotes, process_escaped_chars,
                                 ignore_double_quotes)) {
            ++index;
            continue;
        }
        size_t next_index = index;
        if (callback(index, c, state, next_index) == IterationAction::Break) {
            break;
        }
        index = next_index < index ? index + 1 : next_index + 1;
    }
}

bool extract_trimmed_line(const std::string& line, std::string& trimmed_line,
                          size_t& first_non_space);

std::vector<std::string> tokenize_whitespace(const std::string& input);
size_t find_control_keyword(const std::string& line, const std::string& keyword,
                            size_t search_from = 0);
size_t find_inline_do_position(const std::string& line);
size_t find_inline_done_position(const std::string& line, size_t search_from);
bool check_for_loop_keywords(const std::vector<std::string>& tokens,
                             const std::string& trimmed_line, bool allow_loose_do_detection);
bool inline_loop_body_missing_done(const std::string& trimmed_line);
std::pair<std::vector<std::string>, std::string> tokenize_and_get_first(
    const std::string& trimmed_line);

template <typename Callback>
std::vector<SyntaxError> validate_lines_basic(const std::vector<std::string>& lines,
                                              Callback&& callback) {
    std::vector<SyntaxError> errors;

    for (size_t line_num = 0; line_num < lines.size(); ++line_num) {
        const std::string& line = lines[line_num];
        size_t display_line = line_num + 1;

        std::string trimmed_line;
        size_t first_non_space = 0;
        if (!extract_trimmed_line(line, trimmed_line, first_non_space)) {
            continue;
        }

        auto line_errors = callback(line, trimmed_line, display_line, first_non_space);
        errors.insert(errors.end(), line_errors.begin(), line_errors.end());
    }

    return errors;
}

template <typename ProcessFunc>
std::vector<SyntaxError> process_lines_for_validation(const std::vector<std::string>& lines,
                                                      ProcessFunc process_line_func) {
    return validate_lines_basic(lines, process_line_func);
}

template <typename Callback>
std::vector<SyntaxError> validate_with_tokenized_line(const std::vector<std::string>& lines,
                                                      Callback&& callback) {
    return process_lines_for_validation(
        lines,
        [&](const std::string& line, const std::string& trimmed_line, size_t display_line,
            size_t) -> std::vector<SyntaxError> {
            std::vector<SyntaxError> line_errors;
            auto [tokens, first_token] = tokenize_and_get_first(trimmed_line);
            callback(line_errors, line, trimmed_line, display_line, tokens, first_token);
            return line_errors;
        });
}

size_t adjust_display_line(const std::string& text, size_t base_line, size_t offset);

void append_function_name_errors(std::vector<SyntaxError>& errors, size_t display_line,
                                 const std::string& line, const std::string& func_name,
                                 const std::string& missing_name_suggestion);

struct ForLoopCheckResult {
    bool incomplete = false;
    bool missing_do_keyword = false;
    bool has_inline_do = false;
    bool inline_body_without_done = false;
    std::string header_error;
};

ForLoopCheckResult analyze_for_loop_syntax(const std::vector<std::string>& tokens,
                                           const std::string& trimmed_line);

struct WhileUntilCheckResult {
    bool missing_do_keyword = false;
    bool missing_condition = false;
    bool unclosed_test = false;
    bool has_inline_do = false;
    bool inline_body_without_done = false;
};

WhileUntilCheckResult analyze_while_until_syntax(const std::string& first_token,
                                                 const std::string& trimmed_line,
                                                 const std::vector<std::string>& tokens);

bool next_effective_line_starts_with_keyword(const std::vector<std::string>& lines,
                                             size_t current_index, const std::string& keyword);

struct IfCheckResult {
    bool missing_then_keyword = false;
    bool missing_condition = false;
};

IfCheckResult analyze_if_syntax(const std::vector<std::string>& tokens,
                                const std::string& trimmed_line);

struct CaseCheckResult {
    bool incomplete = false;
    bool missing_in_keyword = false;
};

CaseCheckResult analyze_case_syntax(const std::vector<std::string>& tokens);

bool validate_array_index_expression(const std::string& index_text, std::string& issue);

struct ArithmeticExpansionBounds {
    size_t expr_start;
    size_t expr_end;
    size_t closing_index;
    bool closed;
};

ArithmeticExpansionBounds analyze_arithmetic_expansion_bounds(const std::string& line,
                                                              size_t start);

template <typename Callback>
std::vector<SyntaxError> validate_with_effective_char_iteration(
    const std::vector<std::string>& lines, bool ignore_single_quotes, bool process_escaped_chars,
    Callback&& callback) {
    return process_lines_for_validation(
        lines,
        [&](const std::string& line, const std::string&, size_t display_line,
            size_t) -> std::vector<SyntaxError> {
            std::vector<SyntaxError> line_errors;
            for_each_effective_char(line, ignore_single_quotes, process_escaped_chars,
                                    [&](size_t i, char c, const QuoteState& state,
                                        size_t& next_index) -> IterationAction {
                                        callback(line_errors, line, display_line, i, c, state,
                                                 next_index);
                                        return IterationAction::Continue;
                                    });

            return line_errors;
        });
}

template <typename Callback>
std::vector<SyntaxError> create_char_iteration_validator(const std::vector<std::string>& lines,
                                                         bool ignore_single_quotes,
                                                         bool process_escaped_chars,
                                                         Callback&& callback) {
    return validate_with_effective_char_iteration(
        lines, ignore_single_quotes, process_escaped_chars, std::forward<Callback>(callback));
}

template <typename Callback>
std::vector<SyntaxError> validate_default_char_iteration(const std::vector<std::string>& lines,
                                                         Callback&& callback) {
    return create_char_iteration_validator(lines, false, true, std::forward<Callback>(callback));
}

template <typename Callback>
std::vector<SyntaxError> create_tokenized_validator(const std::vector<std::string>& lines,
                                                    Callback&& callback) {
    return validate_with_tokenized_line(lines, std::forward<Callback>(callback));
}

struct CharIterationContext {
    std::vector<SyntaxError>& line_errors;
    const std::string& line;
    size_t display_line;
    size_t index;
    char character;
    const QuoteState& state;
    size_t& next_index;
};

template <typename Callback>
auto adapt_char_iteration_callback(Callback&& callback) {
    return [callback = std::forward<Callback>(callback)](
               std::vector<SyntaxError>& line_errors, const std::string& line, size_t display_line,
               size_t i, char c, const QuoteState& state, size_t& next_index) {
        CharIterationContext context{line_errors, line, display_line, i, c, state, next_index};
        callback(context);
    };
}

template <typename Callback>
std::vector<SyntaxError> validate_char_iteration_with_context(const std::vector<std::string>& lines,
                                                              bool ignore_single_quotes,
                                                              bool process_escaped_chars,
                                                              Callback&& callback) {
    return create_char_iteration_validator(
        lines, ignore_single_quotes, process_escaped_chars,
        adapt_char_iteration_callback(std::forward<Callback>(callback)));
}

template <typename Callback>
std::vector<SyntaxError> validate_default_char_iteration_with_context(
    const std::vector<std::string>& lines, Callback&& callback) {
    return validate_default_char_iteration(
        lines, adapt_char_iteration_callback(std::forward<Callback>(callback)));
}

template <typename Callback>
std::vector<SyntaxError> validate_char_iteration_ignore_single_quotes(
    const std::vector<std::string>& lines, Callback&& callback) {
    return validate_char_iteration_with_context(lines, true, true,
                                                std::forward<Callback>(callback));
}

struct TokenizedLineContext {
    std::vector<SyntaxError>& line_errors;
    const std::string& line;
    const std::string& trimmed_line;
    size_t display_line;
    const std::vector<std::string>& tokens;
    const std::string& first_token;
    const std::vector<std::string>& all_lines;
    size_t line_index;
};

template <typename Callback>
std::vector<SyntaxError> validate_tokenized_with_first_token_context(
    const std::vector<std::string>& lines, Callback&& callback,
    std::initializer_list<std::string_view> relevant_keywords) {
    std::vector<SyntaxError> errors;

    for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
        const std::string& line = lines[line_idx];
        // The callback only diagnoses these literal first tokens. A substring match
        // may still need tokenization, but absence rules out a relevant token even
        // when the caller has installed a custom whitespace locale.
        if (std::none_of(relevant_keywords.begin(), relevant_keywords.end(),
                         [&line](std::string_view keyword) {
                             return line.find(keyword) != std::string::npos;
                         })) {
            continue;
        }
        std::string trimmed_line;
        size_t first_non_space = 0;
        if (!extract_trimmed_line(line, trimmed_line, first_non_space)) {
            continue;
        }

        auto [tokens, first_token] = tokenize_and_get_first(trimmed_line);
        if (first_token.empty()) {
            continue;
        }

        std::vector<SyntaxError> line_errors;
        TokenizedLineContext context{line_errors, line,        trimmed_line, line_idx + 1,
                                     tokens,      first_token, lines,        line_idx};
        callback(context);
        (void)errors.insert(errors.end(), line_errors.begin(), line_errors.end());
    }

    return errors;
}

}  // namespace shell_validation::internal

#endif  // CJSH_CORE_SRC_VALIDATION_VALIDATION_COMMON_H
