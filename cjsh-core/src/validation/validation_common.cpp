/*
  validation_common.cpp

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

#include "validation_common.h"

#include "error_out.h"
#include "interpreter.h"
#include "interpreter_utils.h"
#include "parser_utils.h"
#include "quote_state.h"
#include "shell_env.h"
#include "string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <locale>
#include <string>
#include <utility>
#include <vector>

using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;
using ErrorCategory = ShellScriptInterpreter::ErrorCategory;

namespace shell_validation::internal {

namespace {

bool is_comment_token(const std::string& token) {
    return !token.empty() && token[0] == '#';
}

template <typename Predicate>
size_t find_char_skipping_escapes(const std::string& text, size_t start_index, Predicate pred) {
    bool escaped = false;

    for (size_t i = start_index; i < text.size(); ++i) {
        char ch = text[i];

        if (escaped) {
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            escaped = true;
            continue;
        }

        if (pred(ch)) {
            return i;
        }
    }

    return std::string::npos;
}

size_t find_matching_backtick_for_validation_impl(const std::string& text, size_t start_index) {
    return find_char_skipping_escapes(text, start_index, [](char ch) { return ch == '`'; });
}

template <typename Func>
void for_each_effective_char_basic(const std::string& text, size_t start_index, Func&& callback) {
    utils::QuoteState state;

    for (size_t i = start_index; i < text.size(); ++i) {
        char ch = text[i];

        if (state.consume_forward(ch) == utils::QuoteAdvanceResult::Continue) {
            continue;
        }

        if (!state.in_single_quote) {
            if (callback(i, ch)) {
                return;
            }
        }
    }
}

bool find_matching_command_substitution_end_for_validation_impl(const std::string& text,
                                                                size_t start_index,
                                                                size_t& end_out) {
    return parser_find_matching_command_substitution_end(text, start_index, end_out);
}

size_t find_marker(const std::string& text, size_t start_pos,
                   const std::string& marker_with_control, size_t marker_with_control_len,
                   const std::string& marker_plain, size_t marker_plain_len,
                   size_t& matched_length) {
    size_t pos_with = (marker_with_control_len > 0) ? text.find(marker_with_control, start_pos)
                                                    : std::string::npos;
    size_t pos_plain =
        (marker_plain_len > 0) ? text.find(marker_plain, start_pos) : std::string::npos;

    if (pos_with == std::string::npos && pos_plain == std::string::npos) {
        matched_length = 0;
        return std::string::npos;
    }

    if (pos_plain != std::string::npos && (pos_with == std::string::npos || pos_plain < pos_with)) {
        matched_length = marker_plain_len;
        return pos_plain;
    }

    matched_length = marker_with_control_len;
    return pos_with;
}

}  // namespace

size_t find_matching_backtick_for_validation(const std::string& text, size_t start_index) {
    return find_matching_backtick_for_validation_impl(text, start_index);
}

bool find_matching_command_substitution_end_for_validation(const std::string& text,
                                                           size_t start_index, size_t& end_out) {
    return find_matching_command_substitution_end_for_validation_impl(text, start_index, end_out);
}

std::string sanitize_command_substitutions_for_validation(const std::string& input) {
    if (input.empty()) {
        return input;
    }

    const std::string& placeholder = substitution_placeholder();
    const std::string& literal_start = subst_literal_start();
    const std::string& literal_end = subst_literal_end();
    const std::string& noenv_start = ::noenv_start();
    const std::string& noenv_end = ::noenv_end();

    std::string output;
    output.reserve(input.size());

    utils::QuoteState quote_state;

    size_t i = 0;
    while (i < input.size()) {
        if (!literal_start.empty() && input.compare(i, literal_start.size(), literal_start) == 0) {
            i += literal_start.size();
            while (i < input.size() && input.compare(i, literal_end.size(), literal_end) != 0) {
                ++i;
            }
            if (i < input.size()) {
                i += literal_end.size();
            }
            (void)output.append(placeholder);
            continue;
        }

        if (!noenv_start.empty() && input.compare(i, noenv_start.size(), noenv_start) == 0) {
            i += noenv_start.size();
            while (i < input.size() && input.compare(i, noenv_end.size(), noenv_end) != 0) {
                ++i;
            }
            if (i < input.size()) {
                i += noenv_end.size();
            }
            (void)output.append(placeholder);
            continue;
        }

        char c = input[i];

        if (quote_state.escaped) {
            output.push_back(c);
            quote_state.escaped = false;
            ++i;
            continue;
        }

        if (c == '\\' && !quote_state.in_single_quote) {
            quote_state.escaped = true;
            output.push_back(c);
            ++i;
            continue;
        }

        if (c == '\'' && !quote_state.in_double_quote) {
            quote_state.in_single_quote = !quote_state.in_single_quote;
            output.push_back(c);
            ++i;
            continue;
        }

        if (c == '"' && !quote_state.in_single_quote) {
            quote_state.in_double_quote = !quote_state.in_double_quote;
            output.push_back(c);
            ++i;
            continue;
        }

        if (!quote_state.in_single_quote && c == '$' && i + 1 < input.size() &&
            input[i + 1] == '(') {
            size_t end_index = 0;
            if (find_matching_command_substitution_end_for_validation_impl(input, i + 2,
                                                                           end_index)) {
                (void)output.append("$(");
                (void)output.append(placeholder);
                output.push_back(')');
                i = end_index + 1;
                continue;
            }
        }

        if (!quote_state.in_single_quote && c == '`') {
            size_t end_index = find_matching_backtick_for_validation_impl(input, i + 1);
            if (end_index != std::string::npos) {
                output.push_back('`');
                (void)output.append(placeholder);
                output.push_back('`');
                i = end_index + 1;
                continue;
            }
        }

        output.push_back(c);
        ++i;
    }

    return output;
}

std::vector<std::string> sanitize_lines_for_validation(const std::vector<std::string>& lines) {
    std::vector<std::string> sanitized = lines;

    bool inside_subst_literal = false;
    bool inside_noenv_literal = false;

    for (std::string& line : sanitized) {
        size_t pos = 0;

        while (pos <= line.size()) {
            if (inside_subst_literal) {
                size_t matched_len = 0;
                size_t end_pos = find_marker(line, pos, subst_literal_end(),
                                             subst_literal_end().size(), subst_literal_end_plain(),
                                             subst_literal_end_plain().size(), matched_len);
                if (end_pos == std::string::npos) {
                    (void)line.erase(pos);
                    break;
                }

                (void)line.erase(pos, (end_pos + matched_len) - pos);
                inside_subst_literal = false;
                continue;
            }

            if (inside_noenv_literal) {
                size_t matched_len = 0;
                size_t end_pos =
                    find_marker(line, pos, noenv_end(), noenv_end().size(), noenv_end_plain(),
                                noenv_end_plain().size(), matched_len);
                if (end_pos == std::string::npos) {
                    (void)line.erase(pos);
                    break;
                }

                (void)line.erase(pos, (end_pos + matched_len) - pos);
                inside_noenv_literal = false;
                continue;
            }

            size_t subst_len = 0;
            size_t subst_pos = find_marker(
                line, pos, subst_literal_start(), subst_literal_start().size(),
                subst_literal_start_plain(), subst_literal_start_plain().size(), subst_len);

            size_t noenv_len = 0;
            size_t noenv_pos =
                find_marker(line, pos, noenv_start(), noenv_start().size(), noenv_start_plain(),
                            noenv_start_plain().size(), noenv_len);

            if (subst_pos == std::string::npos && noenv_pos == std::string::npos) {
                break;
            }

            bool handle_subst = (noenv_pos == std::string::npos) ||
                                (subst_pos != std::string::npos && subst_pos <= noenv_pos);

            if (handle_subst) {
                (void)line.replace(subst_pos, subst_len, substitution_placeholder());
                pos = subst_pos + substitution_placeholder().size();

                size_t matched_len = 0;
                size_t end_pos = find_marker(line, pos, subst_literal_end(),
                                             subst_literal_end().size(), subst_literal_end_plain(),
                                             subst_literal_end_plain().size(), matched_len);
                if (end_pos == std::string::npos) {
                    (void)line.erase(pos);
                    inside_subst_literal = true;
                    break;
                }

                (void)line.erase(pos, (end_pos + matched_len) - pos);
            } else {
                (void)line.replace(noenv_pos, noenv_len, substitution_placeholder());
                pos = noenv_pos + substitution_placeholder().size();

                size_t matched_len = 0;
                size_t end_pos =
                    find_marker(line, pos, noenv_end(), noenv_end().size(), noenv_end_plain(),
                                noenv_end_plain().size(), matched_len);
                if (end_pos == std::string::npos) {
                    (void)line.erase(pos);
                    inside_noenv_literal = true;
                    break;
                }

                (void)line.erase(pos, (end_pos + matched_len) - pos);
            }
        }
    }

    return sanitized;
}

bool starts_with_keyword_token(const std::string& line, const std::string& keyword) {
    return parser_starts_with_keyword_token(line, keyword, true);
}

std::string extract_identifier_from_token(const std::string& token) {
    size_t start = 0;
    while (start < token.size() && !is_valid_identifier_start(token[start])) {
        ++start;
    }
    if (start >= token.size()) {
        return "";
    }
    size_t end = start + 1;
    while (end < token.size() && is_valid_identifier_char(token[end])) {
        ++end;
    }
    return token.substr(start, end - start);
}

bool is_keyword_token(const std::string& token, const std::string& keyword) {
    if (token.size() < keyword.size()) {
        return false;
    }

    if (token.compare(0, keyword.size(), keyword) != 0) {
        return false;
    }

    for (size_t i = keyword.size(); i < token.size(); ++i) {
        if (token[i] != ';') {
            return false;
        }
    }

    return true;
}

bool is_do_token(const std::string& token) {
    return is_keyword_token(token, "do");
}

bool is_done_token(const std::string& token) {
    return is_keyword_token(token, "done");
}

std::string get_last_non_comment_token(const std::vector<std::string>& tokens) {
    std::string last;
    for (const auto& token : tokens) {
        if (is_comment_token(token)) {
            break;
        }

        if (!token.empty()) {
            last = token;
        }
    }

    return last;
}

bool should_process_char(QuoteState& state, char c, bool ignore_single_quotes,
                         bool process_escaped_chars, bool ignore_double_quotes) {
    if (state.escaped) {
        state.escaped = false;
        return process_escaped_chars;
    }

    if (c == '\\' && (!state.in_quotes || state.quote_char != '\'')) {
        state.escaped = true;
        return false;
    }

    if (!state.in_quotes && (c == '"' || c == '\'')) {
        state.in_quotes = true;
        state.quote_char = c;
        return false;
    }

    if (state.in_quotes && c == state.quote_char) {
        state.in_quotes = false;
        state.quote_char = '\0';
        return false;
    }

    if (state.in_quotes && ((state.quote_char == '\'' && ignore_single_quotes) ||
                            (state.quote_char == '"' && ignore_double_quotes))) {
        return false;
    }

    return true;
}

bool extract_trimmed_line(const std::string& line, std::string& trimmed_line,
                          size_t& first_non_space) {
    first_non_space = line.find_first_not_of(" \t");
    if (first_non_space == std::string::npos) {
        return false;
    }

    if (line[first_non_space] == '#') {
        return false;
    }

    trimmed_line = sanitize_command_substitutions_for_validation(line.substr(first_non_space));
    return true;
}

std::vector<std::string> tokenize_whitespace(const std::string& input) {
    std::vector<std::string> tokens;
    // Match formatted stream extraction's locale without constructing a stream
    // or copying each token through an intermediate string.
    const std::locale locale;
    const auto& ctype = std::use_facet<std::ctype<char>>(locale);
    const char* current = input.data();
    const char* end = current + input.size();
    while (current != end) {
        current = ctype.scan_not(std::ctype_base::space, current, end);
        if (current == end) {
            break;
        }
        const char* token_end = ctype.scan_is(std::ctype_base::space, current, end);
        tokens.emplace_back(current, static_cast<size_t>(token_end - current));
        current = token_end;
    }
    return tokens;
}

bool is_word_boundary(const std::string& text, size_t start, size_t length) {
    return parser_is_word_boundary(text, start, length);
}

size_t find_control_keyword(const std::string& line, const std::string& keyword,
                            size_t search_from) {
    return parser_find_keyword_token(line, keyword, search_from);
}

size_t find_inline_do_position(const std::string& line) {
    return find_control_keyword(line, "do");
}

size_t find_inline_done_position(const std::string& line, size_t search_from) {
    return find_control_keyword(line, "done", search_from);
}

bool check_for_loop_keywords(const std::vector<std::string>& tokens,
                             const std::string& trimmed_line, bool allow_loose_do_detection) {
    if (allow_loose_do_detection) {
        for (const auto& token : tokens) {
            if (is_do_token(token)) {
                return true;
            }
        }
    }
    size_t inline_do_pos = find_inline_do_position(trimmed_line);
    return inline_do_pos != std::string::npos;
}

std::pair<std::vector<std::string>, std::string> tokenize_and_get_first(
    const std::string& trimmed_line) {
    std::vector<std::string> tokens = tokenize_whitespace(trimmed_line);
    std::string first_token = tokens.empty() ? "" : tokens[0];
    return {std::move(tokens), std::move(first_token)};
}

void append_function_name_errors(std::vector<SyntaxError>& errors, size_t display_line,
                                 const std::string& line, const std::string& func_name,
                                 const std::string& missing_name_suggestion) {
    if (func_name.empty() || func_name == "()") {
        errors.push_back(SyntaxError(
            {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::SYNTAX, "FUNC001",
            "Function declaration missing name", line, missing_name_suggestion));
        return;
    }

    if (!is_valid_identifier_start(func_name[0])) {
        errors.push_back(SyntaxError(
            {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::SYNTAX, "FUNC002",
            "Invalid function name '" + func_name + "' - must start with letter or underscore",
            line, "Use valid function name starting with letter or underscore"));
        return;
    }

    for (char c : func_name) {
        if (!is_valid_identifier_char(c)) {
            errors.push_back(SyntaxError(
                {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::SYNTAX, "FUNC002",
                "Invalid function name '" + func_name + "' - contains invalid character '" +
                    std::string(1, c) + "'",
                line, "Use only letters, numbers, and underscores in function names"));
            return;
        }
    }
}

size_t adjust_display_line(const std::string& text, size_t base_line, size_t offset) {
    size_t limit = std::min(offset, text.size());
    std::string::const_iterator end_it = text.begin();
    std::advance(end_it, static_cast<std::string::difference_type>(limit));
    return base_line + static_cast<size_t>(std::count(text.begin(), end_it, '\n'));
}

bool inline_loop_body_missing_done(const std::string& trimmed_line) {
    const size_t inline_do_pos = find_inline_do_position(trimmed_line);
    if (inline_do_pos == std::string::npos) {
        return false;
    }

    const size_t body_start = trimmed_line.find_first_not_of(" \t;", inline_do_pos + 2);
    return body_start != std::string::npos &&
           find_inline_done_position(trimmed_line, body_start) == std::string::npos;
}

ForLoopCheckResult analyze_for_loop_syntax(const std::vector<std::string>& tokens,
                                           const std::string& trimmed_line) {
    ForLoopCheckResult result;

    auto apply_do_checks = [&] {
        bool has_do = check_for_loop_keywords(tokens, trimmed_line, false);
        result.has_inline_do = has_do;
        if (!has_do) {
            result.missing_do_keyword = true;
        }

        if (result.has_inline_do && inline_loop_body_missing_done(trimmed_line)) {
            result.inline_body_without_done = true;
        }
    };

    auto is_c_style_for_header = [&] {
        if (trimmed_line.rfind("for", 0) != 0) {
            return false;
        }

        size_t pos = 3;
        while (pos < trimmed_line.size() &&
               (std::isspace(static_cast<unsigned char>(trimmed_line[pos])) != 0)) {
            ++pos;
        }

        if (pos + 1 >= trimmed_line.size() || trimmed_line.compare(pos, 2, "((") != 0) {
            return false;
        }

        size_t content_start = 0;
        size_t content_end = 0;
        size_t after_close = 0;
        if (!parser_find_balanced_double_parens(trimmed_line, pos, content_start, content_end,
                                                after_close)) {
            result.incomplete = true;
            return true;
        }
        const size_t do_pos = parser_find_inline_do_position(trimmed_line, after_close);
        std::string trailing = trim(strip_inline_comment(trimmed_line.substr(
            after_close, do_pos == std::string::npos ? std::string::npos : do_pos - after_close)));
        if (!trailing.empty() && trailing.back() == ';') {
            trailing.pop_back();
            trailing = trim(trailing);
        }
        if (!trailing.empty()) {
            result.header_error = "invalid C-style loop header: unexpected text after '))'";
        }

        std::string c_style_expr = trimmed_line.substr(content_start, content_end - content_start);
        int semicolon_count = 0;
        int paren_depth = 0;
        bool in_single = false;
        bool in_double = false;
        bool escaped = false;

        for (char ch : c_style_expr) {
            if (escaped) {
                escaped = false;
                continue;
            }

            if (ch == '\\' && !in_single) {
                escaped = true;
                continue;
            }

            if (!in_double && ch == '\'') {
                in_single = !in_single;
                continue;
            }

            if (!in_single && ch == '"') {
                in_double = !in_double;
                continue;
            }

            if (in_single || in_double) {
                continue;
            }

            if (ch == '(') {
                ++paren_depth;
                continue;
            }

            if (ch == ')') {
                if (paren_depth > 0) {
                    --paren_depth;
                }
                continue;
            }

            if (ch == ';' && paren_depth == 0) {
                ++semicolon_count;
            }
        }

        if (semicolon_count != 2) {
            result.header_error =
                "invalid C-style loop header; expected ((init; condition; update))";
        }

        return true;
    };

    if (is_c_style_for_header()) {
        apply_do_checks();
        return result;
    }

    const size_t do_pos = find_inline_do_position(trimmed_line);
    const auto header = parse_named_loop_header(
        strip_inline_comment(trimmed_line.substr(0, do_pos)), "for", config::is_posix_mode());
    result.header_error = header.error;

    apply_do_checks();

    return result;
}

WhileUntilCheckResult analyze_while_until_syntax(const std::string& first_token,
                                                 const std::string& trimmed_line,
                                                 const std::vector<std::string>& tokens) {
    WhileUntilCheckResult result;

    bool has_do = check_for_loop_keywords(tokens, trimmed_line, true);
    std::string last_token = get_last_non_comment_token(tokens);
    result.has_inline_do = is_do_token(last_token);
    if (!has_do) {
        result.missing_do_keyword = true;
    }

    bool found_do = false;
    bool inline_body_present = false;
    bool has_done_after_do = false;

    for (const auto& token : tokens) {
        if (!found_do) {
            if (is_do_token(token)) {
                found_do = true;
            }
            continue;
        }

        if (token.empty()) {
            continue;
        }
        if (token[0] == '#') {
            break;
        }
        if (is_done_token(token)) {
            has_done_after_do = true;
            break;
        }
        inline_body_present = true;
    }

    if (found_do && inline_body_present && !has_done_after_do) {
        result.inline_body_without_done = true;
    }

    size_t kw_pos = trimmed_line.find(first_token);
    std::string after_kw =
        kw_pos != std::string::npos ? trimmed_line.substr(kw_pos + first_token.size()) : "";
    size_t non = after_kw.find_first_not_of(" \t");
    if (non != std::string::npos) {
        after_kw = after_kw.substr(non);
    } else {
        after_kw.clear();
    }

    bool immediate_do =
        (after_kw == "do" || after_kw.find("do ") == 0 || after_kw.find("do\t") == 0);

    size_t semi = after_kw.find(';');
    if (semi != std::string::npos) {
        after_kw = after_kw.substr(0, semi);
    }

    size_t do_pos = after_kw.rfind(" do");
    if (do_pos != std::string::npos && do_pos == after_kw.size() - 3) {
        after_kw = after_kw.substr(0, do_pos);
    }
    do_pos = after_kw.rfind("\tdo");
    if (do_pos != std::string::npos && do_pos == after_kw.size() - 3) {
        after_kw = after_kw.substr(0, do_pos);
    }

    std::string cond = string_utils::trim_right_ascii_whitespace_copy(after_kw);

    if (cond.empty() || immediate_do) {
        result.missing_condition = true;
    } else {
        if ((cond.find('[') != std::string::npos && cond.find(']') == std::string::npos) ||
            (cond.find("[[") != std::string::npos && cond.find("]]") == std::string::npos)) {
            result.unclosed_test = true;
        }
    }

    return result;
}

bool next_effective_line_starts_with_keyword(const std::vector<std::string>& lines,
                                             size_t current_index, const std::string& keyword) {
    for (size_t idx = current_index + 1; idx < lines.size(); ++idx) {
        std::string trimmed_line;
        size_t first_non_space = 0;
        if (!extract_trimmed_line(lines[idx], trimmed_line, first_non_space)) {
            continue;
        }
        return starts_with_keyword_token(trimmed_line, keyword);
    }
    return false;
}

IfCheckResult analyze_if_syntax(const std::vector<std::string>& tokens,
                                const std::string& trimmed_line) {
    IfCheckResult result;

    bool has_then_on_line = find_control_keyword(trimmed_line, "then") != std::string::npos;

    if (!has_then_on_line) {
        result.missing_then_keyword = true;
    }

    if (tokens.size() == 1) {
        result.missing_condition = true;
    } else if (tokens.size() >= 2) {
        const std::string& second = tokens[1];
        if (second == "then" || second.rfind("then", 0) == 0) {
            result.missing_condition = true;
        }
    }

    return result;
}

CaseCheckResult analyze_case_syntax(const std::vector<std::string>& tokens) {
    CaseCheckResult result;

    if (tokens.size() < 3) {
        result.incomplete = true;
    }

    bool has_in_keyword = std::find(tokens.begin(), tokens.end(), "in") != tokens.end();
    if (!has_in_keyword) {
        result.missing_in_keyword = true;
    }

    return result;
}

bool is_allowed_array_index_char(char c) {
    if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_') {
        return true;
    }
    switch (c) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '(':
        case ')':
            return true;
        default:
            return false;
    }
}

bool validate_array_index_expression(const std::string& index_text, std::string& issue) {
    if (index_text.empty()) {
        issue = "Empty array index";
        return false;
    }
    if (index_text.find_first_of(" \t") != std::string::npos) {
        issue = "Array index cannot contain whitespace";
        return false;
    }
    for (char c : index_text) {
        if (!is_allowed_array_index_char(c)) {
            issue = "Invalid characters in array index";
            return false;
        }
    }
    return true;
}

ArithmeticExpansionBounds analyze_arithmetic_expansion_bounds(const std::string& line,
                                                              size_t start) {
    ArithmeticExpansionBounds bounds{};
    bounds.expr_start = start + 3;
    size_t paren_count = 2;
    size_t pos = bounds.expr_start;

    while (pos < line.length() && paren_count > 0) {
        if (line[pos] == '(') {
            paren_count++;
        } else if (line[pos] == ')') {
            paren_count--;
        }
        pos++;
    }

    bounds.closed = paren_count == 0;
    bounds.closing_index = pos;

    size_t expr_end = bounds.closed && pos >= 2 ? pos - 2 : bounds.expr_start;
    if (expr_end < bounds.expr_start) {
        expr_end = bounds.expr_start;
    }
    bounds.expr_end = expr_end;

    return bounds;
}

}  // namespace shell_validation::internal
