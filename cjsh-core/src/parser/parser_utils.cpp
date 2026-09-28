/*
  parser_utils.cpp

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

#include "parser_utils.h"
#include "function_evaluator.h"
#include "interpreter_utils.h"
#include "quote_info.h"
#include "quote_state.h"
#include "string_utils.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

const std::string& subst_literal_start() {
    static const std::string kValue = "\x1E__SUBST_LITERAL_START__\x1E";
    return kValue;
}

const std::string& subst_literal_end() {
    static const std::string kValue = "\x1E__SUBST_LITERAL_END__\x1E";
    return kValue;
}

const std::string& noenv_start() {
    static const std::string kValue = "\x1E__NOENV_START__\x1E";
    return kValue;
}

const std::string& noenv_end() {
    static const std::string kValue = "\x1E__NOENV_END__\x1E";
    return kValue;
}

const std::string& subst_literal_start_plain() {
    static const std::string kValue = "__SUBST_LITERAL_START__";
    return kValue;
}

const std::string& subst_literal_end_plain() {
    static const std::string kValue = "__SUBST_LITERAL_END__";
    return kValue;
}

const std::string& noenv_start_plain() {
    static const std::string kValue = "__NOENV_START__";
    return kValue;
}

const std::string& noenv_end_plain() {
    static const std::string kValue = "__NOENV_END__";
    return kValue;
}

const std::string& substitution_placeholder() {
    static const std::string kValue = "__CJSH_SUBST__";
    return kValue;
}

std::optional<ParserControlToken> parse_parser_control_token(std::string_view token) {
    if (token == "if") {
        return ParserControlToken::If;
    }
    if (token == "then") {
        return ParserControlToken::Then;
    }
    if (token == "elif") {
        return ParserControlToken::Elif;
    }
    if (token == "else") {
        return ParserControlToken::Else;
    }
    if (token == "fi") {
        return ParserControlToken::Fi;
    }
    if (token == "while") {
        return ParserControlToken::While;
    }
    if (token == "until") {
        return ParserControlToken::Until;
    }
    if (token == "for") {
        return ParserControlToken::For;
    }
    if (token == "do") {
        return ParserControlToken::Do;
    }
    if (token == "done") {
        return ParserControlToken::Done;
    }
    if (token == "case") {
        return ParserControlToken::Case;
    }
    if (token == "in") {
        return ParserControlToken::In;
    }
    if (token == "esac") {
        return ParserControlToken::Esac;
    }
    if (token == "function") {
        return ParserControlToken::Function;
    }
    if (token == "{") {
        return ParserControlToken::BraceOpen;
    }
    if (token == "}") {
        return ParserControlToken::BraceClose;
    }

    return std::nullopt;
}

bool parser_control_token_is_opening(ParserControlToken token) {
    switch (token) {
        case ParserControlToken::If:
        case ParserControlToken::For:
        case ParserControlToken::While:
        case ParserControlToken::Until:
        case ParserControlToken::Case:
            return true;
        case ParserControlToken::Then:
        case ParserControlToken::Elif:
        case ParserControlToken::Else:
        case ParserControlToken::Fi:
        case ParserControlToken::Do:
        case ParserControlToken::Done:
        case ParserControlToken::In:
        case ParserControlToken::Esac:
        case ParserControlToken::Function:
        case ParserControlToken::BraceOpen:
        case ParserControlToken::BraceClose:
            return false;
    }

    return false;
}

bool parser_control_token_is_closing(ParserControlToken token) {
    switch (token) {
        case ParserControlToken::Fi:
        case ParserControlToken::Done:
        case ParserControlToken::Esac:
            return true;
        case ParserControlToken::If:
        case ParserControlToken::Then:
        case ParserControlToken::Elif:
        case ParserControlToken::Else:
        case ParserControlToken::While:
        case ParserControlToken::Until:
        case ParserControlToken::For:
        case ParserControlToken::Do:
        case ParserControlToken::Case:
        case ParserControlToken::In:
        case ParserControlToken::Function:
        case ParserControlToken::BraceOpen:
        case ParserControlToken::BraceClose:
            return false;
    }

    return false;
}

bool is_hex_digit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

int from_hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool is_char_escaped(const char* str, size_t pos) {
    if (pos == 0) {
        return false;
    }
    size_t backslash_count = 0;
    size_t i = pos - 1;
    while (true) {
        if (str[i] == '\\') {
            ++backslash_count;
            if (i == 0) {
                break;
            }
            --i;
        } else {
            break;
        }
    }
    return (backslash_count % 2) == 1;
}

bool is_char_escaped(const std::string& str, size_t pos) {
    return is_char_escaped(str.c_str(), pos);
}

bool parser_starts_with_keyword_token(std::string_view text, std::string_view keyword,
                                      bool allow_open_paren_boundary) {
    if (text == keyword) {
        return true;
    }
    if (text.size() <= keyword.size()) {
        return false;
    }
    if (text.compare(0, keyword.size(), keyword) != 0) {
        return false;
    }

    char next = text[keyword.size()];
    return (std::isspace(static_cast<unsigned char>(next)) != 0) ||
           (allow_open_paren_boundary && next == '(');
}

bool parser_is_word_boundary(const std::string& text, size_t start, size_t length) {
    auto is_boundary_char = [](char c) {
        return (std::isspace(static_cast<unsigned char>(c)) != 0) || c == ';' || c == '&' ||
               c == '|' || c == '(' || c == ')' || c == '{' || c == '}';
    };

    if (start > text.size()) {
        return false;
    }

    size_t end = start + length;
    bool start_ok = (start == 0) || is_boundary_char(text[start - 1]);
    bool end_ok = (end >= text.size()) || is_boundary_char(text[end]);
    return start_ok && end_ok;
}

bool parser_is_command_group_brace(const std::string& text, size_t position) {
    if (position >= text.size() || (text[position] != '{' && text[position] != '}')) {
        return false;
    }

    auto is_token_boundary = [](char c) {
        return (std::isspace(static_cast<unsigned char>(c)) != 0) || c == ';' || c == '&' ||
               c == '|' || c == '(' || c == ')';
    };

    if ((position > 0 && !is_token_boundary(text[position - 1])) ||
        (position + 1 < text.size() && !is_token_boundary(text[position + 1]))) {
        return false;
    }

    size_t previous = position;
    bool follows_newline = false;
    while (previous > 0 && std::isspace(static_cast<unsigned char>(text[previous - 1])) != 0) {
        follows_newline = follows_newline || text[previous - 1] == '\n';
        --previous;
    }

    if (previous == 0 || follows_newline) {
        return true;
    }

    char previous_char = text[previous - 1];
    if (previous_char == ';' || previous_char == '&' || previous_char == '|' ||
        previous_char == '(' || previous_char == ')' || previous_char == '{') {
        return true;
    }

    size_t word_start = previous;
    while (word_start > 0 && std::isalpha(static_cast<unsigned char>(text[word_start - 1])) != 0) {
        --word_start;
    }
    std::string_view previous_word(text.data() + word_start, previous - word_start);
    return previous_word == "if" || previous_word == "then" || previous_word == "elif" ||
           previous_word == "else" || previous_word == "do" || previous_word == "while" ||
           previous_word == "until" || previous_word == "time" || previous_char == '!';
}

size_t parser_find_keyword_token(const std::string& text, const std::string& keyword,
                                 size_t search_from) {
    if (keyword.empty() || search_from >= text.size()) {
        return std::string::npos;
    }

    const std::string source = shell_script_interpreter::detail::strip_inline_comment(text);
    utils::QuoteState state;
    bool command_start = true;
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (state.consume_forward(c) == utils::QuoteAdvanceResult::Continue ||
            state.inside_quotes()) {
            command_start = false;
            continue;
        }
        if (c == '$' && i + 1 < source.size() && (source[i + 1] == '(' || source[i + 1] == '{')) {
            const size_t end = source[i + 1] == '(' ? find_matching_paren(source, i + 1)
                                                    : find_matching_brace(source, i + 1);
            if (end != std::string::npos) {
                i = end;
            }
            command_start = false;
            continue;
        }
        if (c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '{' ||
            c == '}') {
            command_start = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        if (command_start && i >= search_from && source.compare(i, keyword.size(), keyword) == 0 &&
            parser_is_word_boundary(source, i, keyword.size())) {
            return i;
        }
        size_t end = i;
        while (end < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[end])) != 0 || source[end] == '_')) {
            ++end;
        }
        if (end > i) {
            const std::string_view word(source.data() + i, end - i);
            command_start = command_start && parser_is_word_boundary(source, i, end - i) &&
                            (word == "if" || word == "then" || word == "elif" || word == "else" ||
                             word == "while" || word == "until" || word == "do");
            i = end - 1;
        } else {
            command_start = false;
        }
    }

    return std::string::npos;
}

size_t parser_find_inline_do_position(const std::string& text, size_t search_from) {
    size_t pos = parser_find_keyword_token(text, "do", search_from);
    while (pos != std::string::npos) {
        size_t prev = pos;
        while (prev > 0 && (std::isspace(static_cast<unsigned char>(text[prev - 1])) != 0)) {
            --prev;
        }
        if (prev > 0 && text[prev - 1] == ';') {
            return pos;
        }
        pos = parser_find_keyword_token(text, "do", pos + 2);
    }
    return std::string::npos;
}

size_t parser_find_block_end(const std::string& text, const std::vector<std::string>& openers,
                             const std::string& closer, int& depth) {
    size_t search_from = 0;
    while (search_from < text.size()) {
        size_t next = parser_find_keyword_token(text, closer, search_from);
        bool closing = true;
        size_t length = closer.size();
        for (const auto& opener : openers) {
            const size_t pos = parser_find_keyword_token(text, opener, search_from);
            if (pos < next) {
                next = pos;
                closing = false;
                length = opener.size();
            }
        }
        if (next == std::string::npos) {
            break;
        }
        depth += closing ? -1 : 1;
        if (depth == 0) {
            return next;
        }
        search_from = next + length;
    }
    return std::string::npos;
}

bool parser_find_matching_command_substitution_end(const std::string& text, size_t start_index,
                                                   size_t& end_out) {
    if (start_index == 0 || start_index > text.size()) {
        return false;
    }

    int depth = 1;
    utils::QuoteState quote_state;

    for (size_t i = start_index; i < text.size(); ++i) {
        char ch = text[i];

        if (quote_state.consume_forward(ch) == utils::QuoteAdvanceResult::Continue) {
            continue;
        }

        if (quote_state.inside_quotes()) {
            continue;
        }

        if (ch == '(') {
            ++depth;
            continue;
        }

        if (ch == ')') {
            --depth;
            if (depth == 0) {
                end_out = i;
                return true;
            }
        }
    }

    return false;
}

bool parser_find_balanced_double_parens(std::string_view text, size_t open_pos,
                                        size_t& content_start_out, size_t& content_end_out,
                                        size_t& after_close_out) {
    if (open_pos + 1 >= text.size() || text.compare(open_pos, 2, "((") != 0) {
        return false;
    }

    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (size_t i = open_pos + 2; i < text.size();) {
        char ch = text[i];

        if (escaped) {
            escaped = false;
            ++i;
            continue;
        }

        if (ch == '\\' && !in_single) {
            escaped = true;
            ++i;
            continue;
        }

        if (!in_double && ch == '\'') {
            in_single = !in_single;
            ++i;
            continue;
        }

        if (!in_single && ch == '"') {
            in_double = !in_double;
            ++i;
            continue;
        }

        if (in_single || in_double) {
            ++i;
            continue;
        }

        if (i + 1 < text.size() && text.compare(i, 2, "((") == 0) {
            ++depth;
            i += 2;
            continue;
        }

        if (i + 1 < text.size() && text.compare(i, 2, "))") == 0) {
            --depth;
            if (depth == 0) {
                content_start_out = open_pos + 2;
                content_end_out = i;
                after_close_out = i + 2;
                return true;
            }
            i += 2;
            continue;
        }

        ++i;
    }

    return false;
}

bool parser_parse_arithmetic_command_form(std::string_view text, bool& negate_status_out,
                                          std::string& expression_out) {
    std::string trimmed = trim_whitespace(std::string(text));
    negate_status_out = false;
    expression_out.clear();

    if (!trimmed.empty() && trimmed.front() == '!') {
        size_t after_bang = 1;
        bool has_whitespace_after_bang =
            (after_bang >= trimmed.size()) ||
            (std::isspace(static_cast<unsigned char>(trimmed[after_bang])) != 0);
        if (has_whitespace_after_bang) {
            negate_status_out = true;
            while (after_bang < trimmed.size() &&
                   (std::isspace(static_cast<unsigned char>(trimmed[after_bang])) != 0)) {
                ++after_bang;
            }
            trimmed = trimmed.substr(after_bang);
        }
    }

    size_t content_start = 0;
    size_t content_end = 0;
    size_t after_close = 0;
    if (!parser_find_balanced_double_parens(trimmed, 0, content_start, content_end, after_close)) {
        return false;
    }

    std::string trailing = trim_whitespace(trimmed.substr(after_close));
    if (!trailing.empty()) {
        return false;
    }

    expression_out = trimmed.substr(content_start, content_end - content_start);
    return true;
}

bool parser_contains_arithmetic_command_form(std::string_view text) {
    utils::ShellQuoteState quote_state;

    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (quote_state.consume_forward(text[i]) == utils::QuoteAdvanceResult::Continue ||
            quote_state.inside_quotes()) {
            continue;
        }

        if (text.compare(i, 2, "((") == 0) {
            char previous = (i == 0) ? '\0' : text[i - 1];
            if (previous != '$') {
                return true;
            }
        }
    }

    return false;
}

std::string trim_trailing_whitespace(const std::string& s) {
    return string_utils::trim_right_ascii_whitespace_copy(s);
}

std::string trim_leading_whitespace(const std::string& s) {
    return string_utils::trim_left_ascii_whitespace_copy(s);
}

std::string trim_whitespace(const std::string& s) {
    return string_utils::trim_ascii_whitespace_copy(s);
}

bool is_valid_identifier_start(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return (std::isalpha(uc) != 0) || c == '_';
}

bool is_valid_identifier_char(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return (std::isalnum(uc) != 0) || c == '_';
}

bool is_valid_identifier(const std::string& name) {
    if (name.empty() || !is_valid_identifier_start(name[0])) {
        return false;
    }
    for (size_t i = 1; i < name.length(); ++i) {
        if (!is_valid_identifier_char(name[i])) {
            return false;
        }
    }
    return true;
}

NamedLoopHeader parse_named_loop_header(std::string header, std::string_view keyword,
                                        bool posix_mode) {
    NamedLoopHeader result;
    header = trim_whitespace(header);
    if (!header.empty() && header.back() == ';' && !is_char_escaped(header, header.size() - 1)) {
        header.pop_back();
        header = trim_whitespace(header);
    }

    size_t pos = keyword.size();
    auto next_word = [&] {
        while (pos < header.size() && std::isspace(static_cast<unsigned char>(header[pos]))) {
            ++pos;
        }
        const size_t start = pos;
        pos = find_token_end_with_quotes(header, pos, header.size(), ";&|<>()");
        if (pos == start && pos < header.size()) {
            ++pos;
        }
        return header.substr(start, pos - start);
    };

    result.variable = next_word();
    if (result.variable.empty()) {
        result.error = "expected a loop variable after '" + std::string(keyword) + "'";
        return result;
    }
    if (!is_valid_identifier(result.variable)) {
        result.error = "invalid loop variable '" + result.variable + "'";
        return result;
    }
    if (!posix_mode && result.variable == "in") {
        result.error = "unexpected 'in' where a loop variable was expected after '" +
                       std::string(keyword) + "'; use a variable name such as 'i'";
        return result;
    }

    const std::string token = next_word();
    if (token.empty()) {
        return result;
    }
    if (token != "in") {
        result.error = "unexpected token '" + token + "' after loop variable '" + result.variable +
                       "'; expected 'in' or the end of the loop header";
        return result;
    }

    result.has_in = true;
    result.words = trim_whitespace(header.substr(pos));
    return result;
}

bool parse_assignment(const std::string& arg, std::string& name, std::string& value,
                      bool strip_surrounding_quotes) {
    if (!split_on_first_equals(arg, name, value, true)) {
        return false;
    }

    if ((strip_surrounding_quotes && value.size() >= 2) &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }

    return true;
}

bool parse_env_assignment(const std::string& arg, std::string& name, std::string& value,
                          bool strip_surrounding_quotes) {
    std::string parsed_name;
    std::string parsed_value;
    if (!parse_assignment(arg, parsed_name, parsed_value, strip_surrounding_quotes)) {
        return false;
    }

    parsed_name = trim_whitespace(parsed_name);
    if (!is_valid_identifier(parsed_name)) {
        return false;
    }

    name = std::move(parsed_name);
    value = std::move(parsed_value);
    return true;
}

bool parse_assignment_operand(const std::string& arg, AssignmentOperand& operand,
                              bool strip_surrounding_quotes) {
    std::string parsed_name;
    std::string parsed_value;
    if (parse_assignment(arg, parsed_name, parsed_value, strip_surrounding_quotes)) {
        operand.name = std::move(parsed_name);
        operand.value = std::move(parsed_value);
        operand.has_assignment = true;
        return true;
    }

    operand.name = arg;
    operand.value.clear();
    operand.has_assignment = false;
    return true;
}

std::string normalize_assignment_target(std::string target, bool& append) {
    append = false;
    if (!target.empty() && target.back() == '+') {
        append = true;
        target.pop_back();
    }
    return target;
}

std::string assignment_target_base_name(const std::string& target) {
    const size_t left_bracket = target.find('[');
    return left_bracket == std::string::npos ? target : target.substr(0, left_bracket);
}

size_t find_closing_parenthesis_token(const std::vector<std::string>& tokens, size_t open_index) {
    int depth = 0;
    for (size_t index = open_index; index < tokens.size(); ++index) {
        if (tokens[index] == "(") {
            ++depth;
        } else if (tokens[index] == ")" && --depth == 0) {
            return index;
        }
    }
    return std::string::npos;
}

size_t find_token_end_with_quotes(const std::string& text, size_t start, size_t end,
                                  const std::string& delimiter_chars, bool stop_on_whitespace) {
    if (start >= end || start >= text.size()) {
        return start;
    }

    utils::QuoteState quote_state;
    size_t i = start;
    while (i < end) {
        char ch = text[i];
        if (ch == '\\' && !quote_state.in_single_quote && i + 1 < end) {
            i += 2;
            continue;
        }
        (void)quote_state.consume_forward(ch);
        if ((!quote_state.inside_quotes()) &&
            ((stop_on_whitespace && (std::isspace(static_cast<unsigned char>(ch)) != 0)) ||
             (!delimiter_chars.empty() && delimiter_chars.find(ch) != std::string::npos))) {
            break;
        }

        ++i;
    }

    return i;
}

bool split_on_first_equals(const std::string& value, std::string& left, std::string& right,
                           bool require_nonempty_left) {
    size_t equals_pos = value.find('=');
    if (equals_pos == std::string::npos) {
        return false;
    }
    if (require_nonempty_left && equals_pos == 0) {
        return false;
    }

    left = value.substr(0, equals_pos);
    right = value.substr(equals_pos + 1);
    return true;
}

namespace {

bool is_assignment_lhs(const std::string& lhs) {
    if (lhs.empty()) {
        return false;
    }

    std::string candidate = lhs;
    if (!candidate.empty() && candidate.back() == '+') {
        candidate.pop_back();
    }
    if (candidate.empty()) {
        return false;
    }

    if (is_valid_identifier(candidate)) {
        return true;
    }

    if (candidate.front() == '[') {
        if (candidate.back() != ']') {
            return false;
        }
        return candidate.size() > 2;
    }

    size_t left_bracket = candidate.find('[');
    if (left_bracket == std::string::npos || candidate.back() != ']') {
        return false;
    }

    std::string name = candidate.substr(0, left_bracket);
    if (!is_valid_identifier(name)) {
        return false;
    }

    return (left_bracket + 2) < candidate.size();
}

}  // namespace

bool looks_like_assignment(const std::string& value) {
    std::string name;
    std::string rhs;
    if (!split_on_first_equals(value, name, rhs, true)) {
        return false;
    }

    return is_assignment_lhs(name);
}

bool parse_here_doc_header(std::string_view text, size_t operator_pos, HereDocHeader& header_out) {
    if (operator_pos >= text.size()) {
        return false;
    }
    if (text.compare(operator_pos, 2, "<<") != 0) {
        return false;
    }

    HereDocHeader parsed;
    parsed.operator_length = 2;
    if (operator_pos + 2 < text.size() && text[operator_pos + 2] == '-') {
        parsed.operator_length = 3;
        parsed.strip_tabs = true;
    }

    size_t delim_start = operator_pos + parsed.operator_length;
    while (delim_start < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[delim_start])) != 0)) {
        ++delim_start;
    }

    size_t delim_end = delim_start;
    while (delim_end < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[delim_end])) == 0)) {
        ++delim_end;
    }

    if (delim_start == delim_end) {
        return false;
    }

    parsed.delimiter_start = delim_start;
    parsed.delimiter_end = delim_end;
    parsed.delimiter = std::string(text.substr(delim_start, delim_end - delim_start));

    if (parsed.delimiter.size() >= 2 &&
        ((parsed.delimiter.front() == '"' && parsed.delimiter.back() == '"') ||
         (parsed.delimiter.front() == '\'' && parsed.delimiter.back() == '\''))) {
        parsed.expand = false;
        parsed.delimiter = parsed.delimiter.substr(1, parsed.delimiter.size() - 2);
    }

    header_out = std::move(parsed);
    return true;
}

std::string trim_here_doc_compare_line(const std::string& line) {
    std::string trimmed = string_utils::trim_left_ascii_whitespace_copy(line);
    return string_utils::trim_right_ascii_whitespace_copy(trimmed);
}

bool has_line_continuation_suffix(const std::string& text, bool trim_newlines) {
    size_t pos = text.size();
    if (trim_newlines) {
        while (pos > 0 && (text[pos - 1] == '\n' || text[pos - 1] == '\r')) {
            --pos;
        }
    }

    while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t')) {
        --pos;
    }

    if (pos == 0 || text[pos - 1] != '\\') {
        return false;
    }

    size_t slash_count = 0;
    while (pos > 0 && text[pos - 1] == '\\') {
        ++slash_count;
        --pos;
    }

    return (slash_count % 2) == 1;
}

std::pair<std::string, bool> strip_noenv_sentinels(const std::string& s) {
    const std::string& start = noenv_start();
    const std::string& end = noenv_end();

    bool changed = false;
    std::string result;
    result.reserve(s.size());

    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, start.size(), start) == 0) {
            i += start.size();
            changed = true;
            continue;
        }
        if (s.compare(i, end.size(), end) == 0) {
            i += end.size();
            changed = true;
            continue;
        }
        result += s[i];
        ++i;
    }

    if (!changed) {
        return {s, false};
    }

    return {result, true};
}

bool strip_subst_literal_markers(std::string& value) {
    const std::string& start = subst_literal_start();
    const std::string& end = subst_literal_end();

    bool changed = false;
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size();) {
        if (value.compare(i, start.size(), start) == 0) {
            i += start.size();
            changed = true;
        } else if (value.compare(i, end.size(), end) == 0) {
            i += end.size();
            changed = true;
        } else {
            result += value[i];
            ++i;
        }
    }

    if (changed) {
        value = std::move(result);
    }

    return changed;
}

namespace {

size_t find_matching_delimiter(const std::string& text, size_t start_pos, char opening,
                               char closing) {
    if (start_pos >= text.length() || text[start_pos] != opening) {
        return std::string::npos;
    }

    const std::string source = shell_script_interpreter::detail::strip_inline_comment(text);
    utils::QuoteState state;
    int depth = 0;
    for (size_t i = start_pos; i < source.length(); ++i) {
        if (state.consume_forward(source[i]) == utils::QuoteAdvanceResult::Continue ||
            state.inside_quotes()) {
            continue;
        }

        if (source[i] == opening) {
            depth++;
        } else if (source[i] == closing) {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }

    return std::string::npos;
}

}  // namespace

size_t find_matching_paren(const std::string& text, size_t start_pos) {
    return find_matching_delimiter(text, start_pos, '(', ')');
}

size_t find_matching_brace(const std::string& text, size_t start_pos) {
    return find_matching_delimiter(text, start_pos, '{', '}');
}

std::vector<std::string> merge_command_group_lines(const std::vector<std::string>& lines) {
    std::vector<std::string> result = lines;
    for (size_t i = 0; i < result.size(); ++i) {
        std::string source = shell_script_interpreter::detail::strip_inline_comment(result[i]);
        size_t first = source.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        auto header = function_evaluator::parse_function_header(source, true);
        size_t end_line = i;
        if (header && header->opening == '\0') {
            size_t next = i + 1;
            for (; next < result.size(); ++next) {
                const std::string next_line =
                    shell_script_interpreter::detail::strip_inline_comment(result[next]);
                const size_t lead = next_line.find_first_not_of(" \t\r\n");
                if (lead == std::string::npos) {
                    continue;
                }
                if (next_line[lead] != '{' && next_line[lead] != '(') {
                    next = result.size();
                }
                break;
            }
            if (next == result.size()) {
                continue;
            }
            while (end_line < next) {
                source += '\n';
                source += result[++end_line];
            }
            source = shell_script_interpreter::detail::strip_inline_comment(source);
            header = function_evaluator::parse_function_header(source);
        }
        size_t body_start = first;
        char opening = source[first];
        if (header && header->opening != '\0') {
            body_start = header->body_start;
            opening = header->opening;
        } else if (opening != '(' &&
                   (opening != '{' || !parser_is_command_group_brace(source, first))) {
            continue;
        }
        auto find_close = [&] {
            return opening == '{' ? find_matching_brace(source, body_start)
                                  : find_matching_paren(source, body_start);
        };
        while (find_close() == std::string::npos && end_line + 1 < result.size()) {
            source += '\n';
            source += result[++end_line];
        }
        if (end_line > i) {
            // Empty placeholders preserve the source line numbers of following commands.
            result[i] = std::move(source);
            for (size_t consumed = i + 1; consumed <= end_line; ++consumed) {
                result[consumed].clear();
            }
            i = end_line;
        }
    }
    return result;
}
