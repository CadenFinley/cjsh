/*
  validation_expressions.cpp

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

#include "interpreter.h"

#include "error_out.h"
#include "parser_utils.h"
#include "shell_env.h"
#include "validation_common.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

using namespace shell_validation::internal;

std::vector<ShellScriptInterpreter::SyntaxError>
ShellScriptInterpreter::validate_arithmetic_expressions(const std::vector<std::string>& lines) {
    return validate_char_iteration_ignore_single_quotes(lines, [](CharIterationContext& ctx) {
        auto& line_errors = ctx.line_errors;
        const std::string& line = ctx.line;

        if (ctx.character == '$' && ctx.index + 2 < line.length() && line[ctx.index + 1] == '(' &&
            line[ctx.index + 2] == '(') {
            const size_t start = ctx.index;
            const auto bounds = analyze_arithmetic_expansion_bounds(line, start);
            const size_t adjusted_line = adjust_display_line(line, ctx.display_line, start);

            if (!bounds.closed) {
                line_errors.push_back(SyntaxError({adjusted_line, start, bounds.closing_index, 0},
                                                  ErrorSeverity::ERROR, ErrorCategory::SYNTAX,
                                                  "ARITH001", "Unclosed arithmetic expansion $(()",
                                                  line, "Add closing ))"));
            } else {
                std::string expr =
                    line.substr(bounds.expr_start, bounds.expr_end - bounds.expr_start);

                if (expr.empty()) {
                    line_errors.push_back(SyntaxError(
                        {adjusted_line, start, bounds.closing_index, 0}, ErrorSeverity::ERROR,
                        ErrorCategory::SYNTAX, "ARITH002", "Empty arithmetic expression", line,
                        "Provide expression inside $(( ))"));
                } else {
                    std::string trimmed_expr = trim_whitespace(expr);

                    if (!trimmed_expr.empty()) {
                        char last_char = trimmed_expr.back();
                        if (last_char == '+' || last_char == '-' || last_char == '*' ||
                            last_char == '/' || last_char == '%' || last_char == '&' ||
                            last_char == '|' || last_char == '^') {
                            line_errors.push_back(SyntaxError(
                                {adjusted_line, start, bounds.closing_index, 0},
                                ErrorSeverity::ERROR, ErrorCategory::SYNTAX, "ARITH003",
                                "Incomplete arithmetic expression - missing operand", line,
                                "Add operand after '" + std::string(1, last_char) + "'"));
                        }
                    }

                    if (expr.find("/0") != std::string::npos ||
                        expr.find("% 0") != std::string::npos) {
                        line_errors.push_back(SyntaxError(
                            {adjusted_line, start, bounds.closing_index, 0}, ErrorSeverity::WARNING,
                            ErrorCategory::SEMANTICS, "ARITH004", "Potential division by zero",
                            line, "Ensure divisor is not zero"));
                    }

                    int balance = 0;
                    for (char ec : expr) {
                        if (ec == '(') {
                            balance++;
                        } else if (ec == ')') {
                            balance--;
                        }
                        if (balance < 0) {
                            break;
                        }
                    }
                    if (balance != 0) {
                        line_errors.push_back(
                            SyntaxError({ctx.display_line, start, bounds.closing_index, 0},
                                        ErrorSeverity::ERROR, ErrorCategory::SYNTAX, "ARITH005",
                                        "Unbalanced parentheses in arithmetic expression", line,
                                        "Check parentheses balance in expression"));
                    }
                }
            }

            ctx.next_index = (bounds.closing_index == 0) ? ctx.index : bounds.closing_index - 1;
        }

        if (ctx.character == '$' && ctx.index + 1 < line.length() && line[ctx.index + 1] == '[') {
            line_errors.push_back(SyntaxError({ctx.display_line, ctx.index, ctx.index + 2, 0},
                                              ErrorSeverity::WARNING, ErrorCategory::STYLE,
                                              "ARITH006",
                                              "Deprecated arithmetic syntax $[...], use $((...))",
                                              line, "Replace $[expr] with $((expr))"));
        }
    });
}

std::vector<ShellScriptInterpreter::SyntaxError>
ShellScriptInterpreter::validate_parameter_expansions(const std::vector<std::string>& lines) {
    return validate_char_iteration_ignore_single_quotes(lines, [](CharIterationContext& ctx) {
        auto& line_errors = ctx.line_errors;
        const std::string& line = ctx.line;
        size_t display_line = ctx.display_line;
        size_t i = ctx.index;
        char c = ctx.character;
        const QuoteState& state = ctx.state;
        size_t& next_index = ctx.next_index;

        if (config::posix_mode && c == '$' && i + 1 < line.size() && line[i + 1] == '{') {
            const size_t end = find_matching_brace(line, i + 1);
            if (end != std::string::npos &&
                posix_parameter_name_end(line.substr(i + 2, end - i - 2)) == std::string::npos) {
                line_errors.push_back(SyntaxError(
                    {display_line, i, end + 1, 0}, ErrorSeverity::ERROR, ErrorCategory::SYNTAX,
                    "POSIX014", "Parameter expansion is disabled in POSIX mode", line,
                    "Use POSIX parameter operators"));
            }
        }

        if (c == '$' && i + 1 < line.length() && line[i + 1] == '(') {
            size_t start = i;
            size_t end = 0;
            bool closed = find_matching_command_substitution_end_for_validation(line, i + 2, end);
            size_t j = closed ? end + 1 : line.length();

            if (!closed) {
                line_errors.push_back(SyntaxError({display_line, start, j, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::SYNTAX, "SYN005",
                                                  "Unclosed command substitution $() - missing ')'",
                                                  line, "Add closing parenthesis"));
            }

            next_index = j - 1;
        }

        if (c == '`' && !state.in_quotes) {
            size_t start = i;
            size_t closing = find_matching_backtick_for_validation(line, i + 1);
            bool found_closing = closing != std::string::npos;
            size_t j = found_closing ? closing + 1 : line.length();

            if (!found_closing) {
                line_errors.push_back(SyntaxError(
                    {display_line, start, j, 0}, ErrorSeverity::ERROR, ErrorCategory::SYNTAX,
                    "SYN006", "Unclosed backtick command substitution - missing '`'", line,
                    "Add closing backtick"));
            }

            next_index = j - 1;
        }

        if (!state.in_quotes && c == '=' && i > 0) {
            size_t var_start = i;

            if (i > 0 && line[i - 1] == ']') {
                size_t pos = i - 1;
                int bracket_depth = 0;
                bool found_open = false;
                while (pos > 0) {
                    char bc = line[pos];
                    if (bc == ']') {
                        bracket_depth++;
                    } else if (bc == '[') {
                        if (bracket_depth == 0) {
                            found_open = true;
                            break;
                        }
                        bracket_depth--;
                    }
                    pos--;
                }
                if (found_open) {
                    size_t name_end = pos;
                    size_t name_start = name_end;
                    while (name_start > 0 &&
                           (std::isalnum(line[name_start - 1]) || line[name_start - 1] == '_')) {
                        name_start--;
                    }
                    if (name_start < name_end) {
                        size_t index_start = pos + 1;
                        size_t index_end = i - 2;
                        std::string index_text;
                        if (index_end >= index_start) {
                            index_text = line.substr(index_start, index_end - index_start + 1);
                        }
                        std::string var_name_only = line.substr(name_start, name_end - name_start);

                        std::string index_issue;
                        if (!validate_array_index_expression(index_text, index_issue)) {
                            line_errors.push_back(SyntaxError(
                                {display_line, name_start, i, 0}, ErrorSeverity::ERROR,
                                ErrorCategory::VARIABLES, "VAR005",
                                index_issue + " for array '" + var_name_only + "'", line,
                                "Use a valid numeric or arithmetic expression index"));
                        }

                        var_start = name_start;
                    }
                }
            }

            while (var_start > 0 &&
                   (std::isalnum(line[var_start - 1]) || line[var_start - 1] == '_')) {
                var_start--;
            }

            if (var_start < i) {
                std::string var_name = line.substr(var_start, i - var_start);

                if (!var_name.empty()) {
                    std::string line_prefix = line.substr(0, var_start);
                    size_t first_word_end = line_prefix.find_first_of(" \t");
                    std::string first_word = (first_word_end != std::string::npos)
                                                 ? line_prefix.substr(0, first_word_end)
                                                 : line_prefix;

                    size_t start_pos = first_word.find_first_not_of(" \t");
                    if (start_pos != std::string::npos) {
                        first_word = first_word.substr(start_pos);
                    }

                    if (first_word == "export" || first_word == "alias" || first_word == "local" ||
                        first_word == "declare" || first_word == "typeset" ||
                        first_word == "readonly") {
                        return;
                    }

                    if (!is_valid_identifier_start(var_name[0])) {
                        line_errors.push_back(SyntaxError(
                            {display_line, var_start, i, 0}, ErrorSeverity::ERROR,
                            ErrorCategory::VARIABLES, "VAR004",
                            "Invalid variable name '" + var_name +
                                "' - must start with letter or underscore",
                            line, "Use variable name starting with letter or underscore"));
                    }

                    if (var_start == 0 ||
                        line.substr(0, var_start).find_first_not_of(" \t") == std::string::npos) {
                        if (i > 0 && std::isspace(line[i - 1])) {
                            line_errors.push_back(
                                SyntaxError({display_line, i - 1, i + 1, 0}, ErrorSeverity::ERROR,
                                            ErrorCategory::VARIABLES, "VAR005",
                                            "Variable assignment cannot have spaces around '='",
                                            line, "Remove spaces: " + var_name + "=value"));
                        }
                        if (i + 1 < line.length() && std::isspace(line[i + 1])) {
                            line_errors.push_back(SyntaxError(
                                {display_line, var_start, i + 2, 0}, ErrorSeverity::ERROR,
                                ErrorCategory::VARIABLES, "VAR005",
                                "Variable assignment cannot have spaces around '='", line,
                                "Remove spaces: " + var_name + "=value"));
                        }
                    }
                }
            }
        }
    });
}

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::validate_array_syntax(
    const std::vector<std::string>& lines) {
    return validate_default_char_iteration_with_context(lines, [](CharIterationContext& ctx) {
        auto& line_errors = ctx.line_errors;
        const std::string& line = ctx.line;
        size_t display_line = ctx.display_line;
        size_t i = ctx.index;
        char c = ctx.character;
        const QuoteState& state = ctx.state;
        size_t& next_index = ctx.next_index;

        if (!state.in_quotes && c == '(' && i > 0) {
            size_t var_end = i;
            while (var_end > 0 && std::isspace(static_cast<unsigned char>(line[var_end - 1]))) {
                var_end--;
            }

            if (var_end > 0 && line[var_end - 1] == '=') {
                if (config::posix_mode) {
                    line_errors.push_back(SyntaxError(
                        {display_line, var_end - 1, i + 1, 0}, ErrorSeverity::ERROR,
                        ErrorCategory::SYNTAX, "POSIX005", "Arrays are disabled in POSIX mode",
                        line, "Use separate scalar variables or positional parameters"));
                    next_index = line.length();
                    return;
                }

                size_t paren_count = 1;
                size_t j = i + 1;
                QuoteState nested_state;

                while (j < line.length() && paren_count > 0) {
                    char inner_char = line[j];

                    if (!should_process_char(nested_state, inner_char, false)) {
                        ++j;
                        continue;
                    }

                    if (!nested_state.in_quotes) {
                        if (inner_char == '(') {
                            paren_count++;
                        } else if (inner_char == ')') {
                            paren_count--;
                        }
                    }
                    ++j;
                }

                if (paren_count > 0) {
                    line_errors.push_back(SyntaxError({display_line, i, j, 0}, ErrorSeverity::ERROR,
                                                      ErrorCategory::SYNTAX, "SYN009",
                                                      "Unclosed array declaration - missing ')'",
                                                      line, "Add closing parenthesis"));
                }

                if (j > 0) {
                    next_index = j - 1;
                }
            }
        }
    });
}
