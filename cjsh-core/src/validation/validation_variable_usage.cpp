/*
  validation_variable_usage.cpp

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

#include <algorithm>
#include <iterator>
#include "error_out.h"
#include "interpreter.h"

#include "interpreter_utils.h"
#include "parser_utils.h"
#include "quote_state.h"
#include "validation_common.h"

#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using shell_script_interpreter::detail::should_skip_line;
using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;
using namespace shell_validation::internal;

namespace {

enum class SeparatorToken : std::uint8_t {
    Newline,
    Semicolon,
    DoubleSemicolon,
    Pipe,
    Or,
    Amp,
    AmpCaret,
    AmpCaretBang,
    And,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Do,
    Then,
    Elif,
    Fi,
    Done
};

const char* separator_token_text(SeparatorToken token) {
    switch (token) {
        case SeparatorToken::Newline:
            return "\n";
        case SeparatorToken::Semicolon:
            return ";";
        case SeparatorToken::DoubleSemicolon:
            return ";;";
        case SeparatorToken::Pipe:
            return "|";
        case SeparatorToken::Or:
            return "||";
        case SeparatorToken::Amp:
            return "&";
        case SeparatorToken::AmpCaret:
            return "&^";
        case SeparatorToken::AmpCaretBang:
            return "&^!";
        case SeparatorToken::And:
            return "&&";
        case SeparatorToken::LParen:
            return "(";
        case SeparatorToken::RParen:
            return ")";
        case SeparatorToken::LBrace:
            return "{";
        case SeparatorToken::RBrace:
            return "}";
        case SeparatorToken::Do:
            return "do";
        case SeparatorToken::Then:
            return "then";
        case SeparatorToken::Elif:
            return "elif";
        case SeparatorToken::Fi:
            return "fi";
        case SeparatorToken::Done:
            return "done";
    }
    return "";
}

struct TokenInfo {
    std::string text;
    size_t start;
    size_t end;
};

std::vector<TokenInfo> tokenize_shell_segment(const std::string& text, size_t start, size_t end) {
    std::vector<TokenInfo> tokens;
    if (start >= end || start >= text.size()) {
        return tokens;
    }

    size_t i = start;
    while (i < end) {
        while (i < end && text[i] != '\n' &&
               (std::isspace(static_cast<unsigned char>(text[i])) != 0)) {
            ++i;
        }
        if (i >= end) {
            break;
        }

        if (i + 3 <= end) {
            const std::string three_chars = text.substr(i, 3);
            if (three_chars == "&^!") {
                tokens.push_back({three_chars, i, i + 3});
                i += 3;
                continue;
            }
        }

        if (i + 2 <= end) {
            const std::string two_chars = text.substr(i, 2);
            if (two_chars == "&&" || two_chars == "||" || two_chars == ";;" || two_chars == "&^") {
                tokens.push_back({two_chars, i, i + 2});
                i += 2;
                continue;
            }
        }

        if (text[i] == '\n' || text[i] == ';' || text[i] == '|' || text[i] == '&' ||
            text[i] == '(' || text[i] == ')' || text[i] == '{' || text[i] == '}') {
            tokens.push_back({std::string(1, text[i]), i, i + 1});
            ++i;
            continue;
        }

        size_t token_start = i;
        utils::ShellQuoteState state;
        while (i < end) {
            const char c = text[i];
            // Expansions are part of a word; their operators cannot start a new command here.
            if (!state.escaped && !state.in_single_quote && c == '$' && i + 1 < end &&
                (text[i + 1] == '(' || text[i + 1] == '{')) {
                const size_t close = text[i + 1] == '(' ? find_matching_paren(text, i + 1)
                                                        : find_matching_brace(text, i + 1);
                i = close == std::string::npos ? end : std::min(close + 1, end);
                continue;
            }
            if (state.consume_forward(c) == utils::QuoteAdvanceResult::Process &&
                !state.inside_quotes() &&
                (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ';' || c == '|' ||
                 c == '&' || c == '(' || c == ')' || c == '{' || c == '}')) {
                break;
            }
            ++i;
        }
        tokens.push_back({text.substr(token_start, i - token_start), token_start, i});
    }

    return tokens;
}

bool is_command_separator_token(const std::string& token) {
    static const SeparatorToken separators[] = {
        SeparatorToken::Newline,  SeparatorToken::Semicolon,    SeparatorToken::DoubleSemicolon,
        SeparatorToken::Pipe,     SeparatorToken::Or,           SeparatorToken::Amp,
        SeparatorToken::AmpCaret, SeparatorToken::AmpCaretBang, SeparatorToken::And,
        SeparatorToken::LParen,   SeparatorToken::RParen,       SeparatorToken::LBrace,
        SeparatorToken::RBrace,   SeparatorToken::Do,           SeparatorToken::Then,
        SeparatorToken::Elif,     SeparatorToken::Fi,           SeparatorToken::Done};
    return std::any_of(std::begin(separators), std::end(separators),
                       [&](const auto sep) { return token == separator_token_text(sep); });
}

bool is_special_shell_variable(const std::string& name) {
    static const std::unordered_set<std::string> kSpecialVars = {
        "IFS",        "PATH",     "HOME",        "PWD",           "OLDPWD",     "MAIL",
        "MAILPATH",   "PS1",      "PS2",         "PS3",           "PS4",        "PS5",
        "PS6",        "LANG",     "LC_ALL",      "LC_CTYPE",      "LC_COLLATE", "LC_MESSAGES",
        "LC_NUMERIC", "OPTIND",   "OPTARG",      "SECONDS",       "RANDOM",     "LINENO",
        "HISTFILE",   "HISTSIZE", "HISTCONTROL", "PROMPT_COMMAND"};
    return kSpecialVars.find(name) != kSpecialVars.end();
}

bool is_assignment_token(const std::string& token) {
    return !token.empty() && token[0] != '$' && looks_like_assignment(token);
}

std::string normalize_assignment_identifier(const std::string& token) {
    std::string lhs;
    std::string rhs;
    if (!parse_assignment(token, lhs, rhs, false)) {
        return "";
    }

    lhs = trim_whitespace(lhs);

    if (!lhs.empty() && lhs.back() == '+') {
        lhs.pop_back();
    }

    size_t bracket_pos = lhs.find('[');
    if (bracket_pos != std::string::npos) {
        lhs = lhs.substr(0, bracket_pos);
    }

    return lhs;
}

void collect_leading_assignments_from_tokens(
    const std::vector<TokenInfo>& tokens, const std::string& original_line, size_t display_line,
    std::map<std::string, std::vector<size_t>>& defined_vars) {
    bool command_started = false;
    bool in_double_bracket_test = false;

    for (const auto& token : tokens) {
        if (token.text.empty()) {
            continue;
        }

        if (in_double_bracket_test) {
            if (token.text == "]]") {
                in_double_bracket_test = false;
            }
            continue;
        }

        if (token.text == "\n" || token.text == ";" || token.text == ";;" || token.text == "|" ||
            token.text == "||" || token.text == "&" || token.text == "&^" || token.text == "&^!" ||
            token.text == "&&" || token.text == "(" || token.text == ")") {
            command_started = false;
            continue;
        }

        if (!command_started) {
            // Reserved words introduce commands only at command positions, never as arguments.
            if (token.text == "if" || token.text == "elif" || token.text == "else" ||
                token.text == "while" || token.text == "until" || token.text == "then" ||
                token.text == "do" || token.text == "!" || token.text == "{") {
                continue;
            }
            if (is_assignment_token(token.text)) {
                std::string var_name = normalize_assignment_identifier(token.text);
                if (!var_name.empty() && is_valid_identifier(var_name)) {
                    defined_vars[var_name].push_back(
                        adjust_display_line(original_line, display_line, token.start));
                }
                continue;
            }
            in_double_bracket_test = token.text == "[[";
            command_started = true;
        }
    }
}

bool read_option_consumes_argument(const std::string& option) {
    if (option.size() < 2 || option[0] != '-') {
        return false;
    }

    char flag = option[1];
    switch (flag) {
        case 'p':
        case 'u':
        case 't':
        case 'd':
        case 'N':
        case 'n':
        case 'i':
        case 'k':
            return option.size() == 2;
        default:
            return false;
    }
}

bool declaration_option_contains(const std::string& option_token, char target_option) {
    if (option_token.size() < 2 || (option_token[0] != '-' && option_token[0] != '+')) {
        return false;
    }
    return option_token.find(target_option, 1) != std::string::npos;
}

bool is_declaration_command(const std::string& command_name) {
    return command_name == "export" || command_name == "local" || command_name == "declare" ||
           command_name == "typeset" || command_name == "readonly";
}

void collect_declaration_definitions(const std::vector<TokenInfo>& tokens,
                                     const std::string& original_line, size_t display_line,
                                     std::map<std::string, std::vector<size_t>>& defined_vars) {
    size_t idx = 0;
    while (idx < tokens.size()) {
        const auto& token = tokens[idx];
        if (token.text.empty()) {
            ++idx;
            continue;
        }

        if (is_command_separator_token(token.text)) {
            ++idx;
            continue;
        }

        if (is_assignment_token(token.text)) {
            ++idx;
            continue;
        }

        if (!is_declaration_command(token.text)) {
            ++idx;
            while (idx < tokens.size() && !is_command_separator_token(tokens[idx].text)) {
                ++idx;
            }
            continue;
        }

        const bool is_export = token.text == "export";
        bool print_mode = false;
        bool function_mode = false;

        ++idx;
        while (idx < tokens.size()) {
            const std::string& option = tokens[idx].text;
            if (is_command_separator_token(option)) {
                break;
            }

            if (option == "--") {
                ++idx;
                break;
            }

            if (option.size() > 1 && (option[0] == '-' || option[0] == '+')) {
                if (declaration_option_contains(option, 'p')) {
                    print_mode = true;
                }
                if (declaration_option_contains(option, 'f') ||
                    declaration_option_contains(option, 'F')) {
                    function_mode = true;
                }
                ++idx;
                continue;
            }

            break;
        }

        while (idx < tokens.size()) {
            const auto& operand = tokens[idx];
            if (is_command_separator_token(operand.text)) {
                break;
            }

            if (operand.text.empty()) {
                ++idx;
                continue;
            }

            if (is_export && !operand.text.empty() && operand.text[0] == '-') {
                ++idx;
                continue;
            }

            if (print_mode || function_mode) {
                ++idx;
                continue;
            }

            if (operand.text == "(" || operand.text == ")") {
                ++idx;
                continue;
            }

            std::string declared_name = is_assignment_token(operand.text)
                                            ? normalize_assignment_identifier(operand.text)
                                            : extract_identifier_from_token(operand.text);

            if (!declared_name.empty() && is_valid_identifier(declared_name)) {
                defined_vars[declared_name].push_back(
                    adjust_display_line(original_line, display_line, operand.start));
            }

            ++idx;
        }
    }
}

void collect_read_variable_definitions(const std::vector<TokenInfo>& tokens,
                                       const std::string& original_line, size_t display_line,
                                       std::map<std::string, std::vector<size_t>>& defined_vars) {
    size_t idx = 0;
    while (idx < tokens.size()) {
        if (tokens[idx].text != "read") {
            ++idx;
            continue;
        }

        size_t j = idx + 1;
        while (j < tokens.size()) {
            const auto& current = tokens[j];
            if (is_command_separator_token(current.text)) {
                break;
            }

            if (!current.text.empty() && current.text[0] == '-') {
                bool consumes_next = read_option_consumes_argument(current.text);
                ++j;
                if (consumes_next && j < tokens.size() &&
                    !is_command_separator_token(tokens[j].text) && !tokens[j].text.empty() &&
                    tokens[j].text[0] != '-') {
                    ++j;
                }
                continue;
            }

            if (!current.text.empty() && (current.text[0] == '<' || current.text[0] == '>')) {
                ++j;
                continue;
            }

            std::string var_name = extract_identifier_from_token(current.text);
            if (!var_name.empty() && is_valid_identifier(var_name)) {
                defined_vars[var_name].push_back(
                    adjust_display_line(original_line, display_line, current.start));
            }
            ++j;
        }

        idx = j;
    }
}

}  // namespace

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::validate_variable_usage(
    const std::vector<std::string>& lines, bool include_usage) {
    std::vector<SyntaxError> errors;
    std::map<std::string, std::vector<size_t>> defined_vars;
    std::map<std::string, std::vector<size_t>> used_vars;

    for (size_t line_num = 0; line_num < lines.size(); ++line_num) {
        const std::string& original_line = lines[line_num];
        size_t display_line = line_num + 1;

        // Unclosed ${...} is the only blocking diagnostic in this validator.
        if (!include_usage && original_line.find("${") == std::string::npos) {
            continue;
        }

        if (should_skip_line(original_line)) {
            continue;
        }

        std::string line_without_comments = strip_inline_comment(original_line);
        std::string trimmed_line = trim(line_without_comments);
        if (trimmed_line.empty()) {
            continue;
        }

        if (include_usage) {
            if (starts_with_keyword_token(trimmed_line, "for")) {
                auto tokens = tokenize_whitespace(trimmed_line);
                if (tokens.size() >= 2) {
                    std::string loop_var = extract_identifier_from_token(tokens[1]);
                    if (!loop_var.empty() && is_valid_identifier(loop_var)) {
                        size_t var_pos = line_without_comments.find(loop_var);
                        size_t offset = (var_pos != std::string::npos) ? var_pos : 0;
                        defined_vars[loop_var].push_back(
                            adjust_display_line(original_line, display_line, offset));
                    }
                }
            }

            const auto tokens =
                tokenize_shell_segment(line_without_comments, 0, line_without_comments.size());
            collect_declaration_definitions(tokens, original_line, display_line, defined_vars);

            collect_leading_assignments_from_tokens(tokens, original_line, display_line,
                                                    defined_vars);

            collect_read_variable_definitions(tokens, original_line, display_line, defined_vars);
        }

        QuoteState quote_state;
        for (size_t i = 0; i < line_without_comments.length(); ++i) {
            char c = line_without_comments[i];

            if (!should_process_char(quote_state, c, true)) {
                continue;
            }

            if (c == '$' && i + 1 < line_without_comments.length()) {
                if (i + 2 < line_without_comments.length() && line_without_comments[i + 1] == '(' &&
                    line_without_comments[i + 2] == '(') {
                    const auto bounds =
                        analyze_arithmetic_expansion_bounds(line_without_comments, i);

                    if (bounds.closed) {
                        if (include_usage) {
                            std::string expr = line_without_comments.substr(
                                bounds.expr_start, bounds.expr_end - bounds.expr_start);

                            size_t pos = 0;
                            while (pos < expr.length()) {
                                char ec = expr[pos];
                                if (is_valid_identifier_start(ec)) {
                                    size_t start_pos = pos;
                                    pos++;
                                    while (pos < expr.length() &&
                                           is_valid_identifier_char(expr[pos])) {
                                        pos++;
                                    }

                                    std::string token = expr.substr(start_pos, pos - start_pos);
                                    if (!token.empty() && is_valid_identifier(token)) {
                                        used_vars[token].push_back(
                                            adjust_display_line(original_line, display_line,
                                                                bounds.expr_start + start_pos));
                                    }
                                } else {
                                    pos++;
                                }
                            }
                        }
                        i = (bounds.closing_index == 0) ? i : bounds.closing_index - 1;
                        continue;
                    }
                }

                std::string var_name;
                size_t var_start = i + 1;
                size_t var_end = var_start;

                if (line_without_comments[var_start] == '{') {
                    var_start++;
                    var_end = line_without_comments.find('}', var_start);
                    if (var_end != std::string::npos) {
                        if (include_usage) {
                            var_name = line_without_comments.substr(var_start, var_end - var_start);

                            size_t colon_pos = var_name.find(':');
                            if (colon_pos != std::string::npos) {
                                var_name = var_name.substr(0, colon_pos);
                            }
                        }
                    } else {
                        errors.push_back(SyntaxError({display_line, i, i + 2, 0},
                                                     ErrorSeverity::CRITICAL, ErrorCategory::SYNTAX,
                                                     "SYN008", "Unclosed variable expansion ${",
                                                     original_line, "Add closing brace '}'"));
                        continue;
                    }
                } else if (include_usage &&
                           is_valid_identifier_start(line_without_comments[var_start])) {
                    while (var_end < line_without_comments.length() &&
                           is_valid_identifier_char(line_without_comments[var_end])) {
                        var_end++;
                    }
                    var_name = line_without_comments.substr(var_start, var_end - var_start);
                }

                if (!var_name.empty()) {
                    used_vars[var_name].push_back(
                        adjust_display_line(original_line, display_line, i));
                }
            }
        }
    }

    for (const auto& [var_name, usage_lines] : used_vars) {
        const bool defined_in_script = defined_vars.find(var_name) != defined_vars.end();
        const bool known_to_environment = variable_is_set(var_name);

        if ((!defined_in_script && !known_to_environment) &&
            (std::isdigit(static_cast<unsigned char>(var_name[0])) == 0)) {
            for (size_t line : usage_lines) {
                errors.push_back(SyntaxError(
                    {line, 0, 0, 0}, ErrorSeverity::WARNING, ErrorCategory::VARIABLES, "VAR002",
                    "Variable '" + var_name + "' used but not defined in this script", "",
                    "Define the variable before use: " + var_name + "=value"));
            }
        }
    }

    for (const auto& [var_name, def_lines] : defined_vars) {
        if (is_special_shell_variable(var_name)) {
            continue;
        }
        if (used_vars.find(var_name) == used_vars.end()) {
            for (size_t line : def_lines) {
                errors.push_back(SyntaxError({line, 0, 0, 0}, ErrorSeverity::INFO,
                                             ErrorCategory::VARIABLES, "VAR003",
                                             "Variable '" + var_name + "' defined but never used",
                                             "", "Remove unused variable or add usage"));
            }
        }
    }

    return errors;
}
