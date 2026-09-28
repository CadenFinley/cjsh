/*
  validation_control_flow.cpp

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

#include "error_out.h"
#include "interpreter.h"

#include "interpreter_utils.h"
#include "parser_utils.h"
#include "shell_env.h"
#include "validation_common.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;
namespace validation_internal = shell_validation::internal;

using validation_internal::analyze_case_syntax;
using validation_internal::analyze_for_loop_syntax;
using validation_internal::analyze_if_syntax;
using validation_internal::analyze_while_until_syntax;
using validation_internal::append_function_name_errors;
using validation_internal::check_for_loop_keywords;
using validation_internal::create_tokenized_validator;
using validation_internal::find_inline_do_position;
using validation_internal::find_inline_done_position;
using validation_internal::inline_loop_body_missing_done;
using validation_internal::next_effective_line_starts_with_keyword;
using validation_internal::QuoteState;
using validation_internal::should_process_char;
using validation_internal::TokenizedLineContext;
using validation_internal::validate_lines_basic;
using validation_internal::validate_tokenized_with_first_token_context;

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::validate_function_syntax(
    const std::vector<std::string>& lines) {
    return create_tokenized_validator(
        lines, [](std::vector<SyntaxError>& line_errors, const std::string& line,
                  const std::string& trimmed_line, size_t display_line,
                  const std::vector<std::string>&, const std::string&) {
            constexpr size_t function_keyword_length = 8;
            const bool has_function_keyword =
                trimmed_line.compare(0, function_keyword_length, "function") == 0 &&
                (trimmed_line.length() == function_keyword_length ||
                 (trimmed_line.length() > function_keyword_length &&
                  std::isspace(static_cast<unsigned char>(trimmed_line[function_keyword_length])) !=
                      0));

            if (has_function_keyword) {
                size_t name_start = function_keyword_length;
                while (name_start < trimmed_line.length() &&
                       std::isspace(static_cast<unsigned char>(trimmed_line[name_start])) != 0) {
                    ++name_start;
                }

                size_t name_end = name_start;
                while (name_end < trimmed_line.length() &&
                       std::isspace(static_cast<unsigned char>(trimmed_line[name_end])) == 0 &&
                       trimmed_line[name_end] != '(' && trimmed_line[name_end] != '{') {
                    ++name_end;
                }

                const std::string function_name =
                    trimmed_line.substr(name_start, name_end - name_start);
                append_function_name_errors(line_errors, display_line, line, function_name,
                                            function_name.empty()
                                                ? "Add function name: function name() { ... }"
                                                : "Add function name before parentheses");
            }

            size_t paren_pos = trimmed_line.find("()");
            if ((paren_pos != std::string::npos && paren_pos > 0 && !has_function_keyword) &&
                (trimmed_line.find('{', paren_pos) != std::string::npos)) {
                std::string potential_func = trim(trimmed_line.substr(0, paren_pos));
                append_function_name_errors(line_errors, display_line, line, potential_func,
                                            "Add function name before parentheses");
            }
        });
}

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::validate_loop_syntax(
    const std::vector<std::string>& lines) {
    auto validate_header = [](TokenizedLineContext& ctx) {
        auto& line_errors = ctx.line_errors;
        const std::string& line = ctx.line;
        const std::string& trimmed_line = ctx.trimmed_line;
        size_t display_line = ctx.display_line;
        const auto& tokens = ctx.tokens;
        const std::string& first_token = ctx.first_token;

        if (first_token == "for" || first_token == "for;") {
            auto loop_check = analyze_for_loop_syntax(tokens, trimmed_line);
            bool missing_do_effective =
                loop_check.missing_do_keyword &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "do") &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "in");
            if (!loop_check.header_error.empty()) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::CRITICAL, ErrorCategory::CONTROL_FLOW,
                    "SYN002", loop_check.header_error, line,
                    "Use 'for name in words; do ...; done' or 'for name; do ...; done'"));
            } else if (loop_check.incomplete) {
                line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::CONTROL_FLOW, "SYN002",
                                                  "'for' statement incomplete", line,
                                                  "Complete for statement: for var in list; do"));
            } else if (missing_do_effective) {
                line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::CONTROL_FLOW, "SYN002",
                                                  "'for' statement missing 'do' keyword", line,
                                                  "Add 'do' keyword: for var in list; do"));
            } else if (loop_check.inline_body_without_done &&
                       ctx.line_index + 1 == ctx.all_lines.size()) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                    "SYN002", "'for' loop missing closing 'done' after inline body", line,
                    "End inline loop bodies with 'done' or move the body to a new line"));
            }
        } else if (first_token == "select" || first_token == "select;") {
            const size_t do_pos = find_inline_do_position(trimmed_line);
            const auto header = parse_named_loop_header(
                strip_inline_comment(trimmed_line.substr(0, do_pos)), "select", config::posix_mode);
            if (!header.error.empty()) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::CRITICAL, ErrorCategory::CONTROL_FLOW,
                    "SYN002", header.error, line,
                    "Use 'select name in words; do ...; done' or 'select name; do ...; done'"));
                return;
            }

            bool missing_iteration_list = header.has_in && header.words.empty();

            bool has_do = check_for_loop_keywords(tokens, trimmed_line, false);
            bool missing_do_effective =
                !has_do &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "do") &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "in");

            if (missing_iteration_list) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                    "SYN002", "'select' statement missing selection list after 'in'", line,
                    "Add values after 'in' or omit 'in' to select from positional parameters"));
            } else if (missing_do_effective) {
                line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::CONTROL_FLOW, "SYN002",
                                                  "'select' statement missing 'do' keyword", line,
                                                  "Add 'do' keyword: select var in list; do"));
            } else if (has_do && inline_loop_body_missing_done(trimmed_line) &&
                       ctx.line_index + 1 == ctx.all_lines.size()) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                    "SYN002", "'select' loop missing closing 'done' after inline body", line,
                    "End inline select bodies with 'done' or move the body to a new line"));
            }

        } else if (first_token == "while" || first_token == "until") {
            auto loop_check = analyze_while_until_syntax(first_token, trimmed_line, tokens);
            const bool missing_condition = loop_check.missing_condition;
            const bool missing_do =
                loop_check.missing_do_keyword &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "do");

            if (missing_condition && missing_do) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                    "SYN003",
                    "'" + first_token + "' statement missing condition expression and 'do' keyword",
                    line, "Use syntax: " + first_token + " condition; do"));
            } else {
                if (missing_condition) {
                    line_errors.push_back(SyntaxError(
                        {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                        "SYN003", "'" + first_token + "' loop missing condition expression", line,
                        "Add a condition expression before 'do'"));
                } else if (loop_check.unclosed_test) {
                    line_errors.push_back(SyntaxError(
                        {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                        "SYN003", "Unclosed test expression in '" + first_token + "' condition",
                        line, "Close the '[' with ']' or use '[[ ... ]]'"));
                }

                if (missing_do || (loop_check.inline_body_without_done &&
                                   ctx.line_index + 1 == ctx.all_lines.size())) {
                    std::string msg = "'" + first_token + "' statement missing 'do' keyword";
                    if (loop_check.inline_body_without_done && !missing_do) {
                        msg = "'" + first_token + "' loop missing 'done' after inline body";
                    }

                    line_errors.push_back(
                        SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                    ErrorCategory::CONTROL_FLOW, "SYN002", msg, line,
                                    "Add 'do' keyword and close with 'done': " + first_token +
                                        " condition; do ... done"));
                }
            }
        }
    };
    return validate_tokenized_with_first_token_context(lines, validate_header,
                                                       {"for", "select", "while", "until"});
}

std::vector<ShellScriptInterpreter::SyntaxError>
ShellScriptInterpreter::validate_conditional_syntax(const std::vector<std::string>& lines) {
    // conditional validator feeds both syntax reporting and interactive continuation checks
    auto validate_header = [](TokenizedLineContext& ctx) {
        auto& line_errors = ctx.line_errors;
        const std::string& line = ctx.line;
        const std::string& trimmed_line = ctx.trimmed_line;
        size_t display_line = ctx.display_line;
        const auto& tokens = ctx.tokens;
        const std::string& first_token = ctx.first_token;

        if (first_token == "if") {
            // validate if headers so incomplete if statements are detected before execution
            auto if_check = analyze_if_syntax(tokens, trimmed_line);
            const bool missing_then = if_check.missing_then_keyword;
            const bool missing_condition = if_check.missing_condition;
            const bool missing_then_effective =
                missing_then &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "then");

            if (missing_then_effective && missing_condition) {
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::ERROR, ErrorCategory::CONTROL_FLOW,
                    "SYN004", "'if' statement missing condition and 'then' keyword", line,
                    "Use syntax: if [ condition ]; then"));
            } else {
                if (missing_then_effective) {
                    line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                      ErrorCategory::CONTROL_FLOW, "SYN004",
                                                      "'if' statement missing 'then' keyword", line,
                                                      "Add 'then' keyword: if condition; then"));
                }

                if (missing_condition) {
                    line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                      ErrorCategory::CONTROL_FLOW, "SYN004",
                                                      "'if' statement missing condition", line,
                                                      "Add condition: if [ condition ]; then"));
                }
            }
        } else if (first_token == "case") {
            auto case_check = analyze_case_syntax(tokens);
            bool missing_in_effective =
                case_check.missing_in_keyword &&
                !next_effective_line_starts_with_keyword(ctx.all_lines, ctx.line_index, "in");
            if (missing_in_effective) {
                line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::CONTROL_FLOW, "SYN008",
                                                  "'case' statement missing 'in' keyword", line,
                                                  "Add 'in' keyword: case variable in"));
            } else if (case_check.incomplete) {
                line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::ERROR,
                                                  ErrorCategory::CONTROL_FLOW, "SYN008",
                                                  "'case' statement incomplete", line,
                                                  "Complete case statement: case variable in"));
            }
        }
    };
    return validate_tokenized_with_first_token_context(lines, validate_header, {"if", "case"});
}

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::check_style_guidelines(
    const std::vector<std::string>& lines) {
    return validate_lines_basic(
        lines,
        [](const std::string& line, const std::string& trimmed_line, size_t display_line,
           size_t) -> std::vector<SyntaxError> {
            std::vector<SyntaxError> line_errors;

            if (trimmed_line.rfind("if ", 0) == 0 || trimmed_line.rfind("while ", 0) == 0 ||
                trimmed_line.rfind("until ", 0) == 0) {
                int logical_ops = 0;
                int bracket_depth = 0;
                int max_bracket_depth = 0;
                QuoteState quote_state;

                for (size_t i = 0; i < line.length() - 1; ++i) {
                    char c = line[i];

                    if (!should_process_char(quote_state, c, false, false)) {
                        continue;
                    }

                    if (!quote_state.in_quotes) {
                        if ((c == '&' && line[i + 1] == '&') || (c == '|' && line[i + 1] == '|')) {
                            logical_ops++;
                            i++;
                        } else if (c == '[') {
                            bracket_depth++;
                            max_bracket_depth = std::max(max_bracket_depth, bracket_depth);
                        } else if (c == ']') {
                            bracket_depth--;
                        }
                    }
                }

                if (logical_ops > 3) {
                    line_errors.push_back(SyntaxError(
                        {display_line, 0, 0, 0}, ErrorSeverity::INFO, ErrorCategory::STYLE,
                        "STYLE001",
                        "Complex condition with " + std::to_string(logical_ops) +
                            " logical operators",
                        line, "Consider breaking into multiple if statements or using a function"));
                }

                if (max_bracket_depth > 2) {
                    line_errors.push_back(SyntaxError({display_line, 0, 0, 0}, ErrorSeverity::INFO,
                                                      ErrorCategory::STYLE, "STYLE002",
                                                      "Deeply nested test conditions (depth: " +
                                                          std::to_string(max_bracket_depth) + ")",
                                                      line,
                                                      "Consider simplifying the condition logic"));
                }
            }

            if (line.length() > 100) {
                line_errors.push_back(
                    SyntaxError({display_line, 100, line.length(), 0}, ErrorSeverity::INFO,
                                ErrorCategory::STYLE,
                                "Line length (" + std::to_string(line.length()) +
                                    " chars) exceeds recommended 100 characters",
                                line, "Consider breaking long lines for better readability"));
            }

            if (line.find('\t') != std::string::npos && line.find(' ') != std::string::npos) {
                size_t first_tab = line.find('\t');
                size_t first_space = line.find(' ');
                if (first_tab < 20 && first_space < 20) {
                    line_errors.push_back(SyntaxError(
                        {display_line, 0, std::min(first_tab, first_space), 0}, ErrorSeverity::INFO,
                        ErrorCategory::STYLE, "STYLE004", "Mixed tabs and spaces for indentation",
                        line, "Use consistent indentation (either all tabs or all spaces)"));
                }
            }

            if (trimmed_line.find("eval ") != std::string::npos ||
                trimmed_line.find("$(") != std::string::npos) {
                std::string warning_type = trimmed_line.find("eval ") != std::string::npos
                                               ? "eval"
                                               : "command substitution";
                line_errors.push_back(SyntaxError(
                    {display_line, 0, 0, 0}, ErrorSeverity::WARNING, ErrorCategory::STYLE,
                    "STYLE005", "Use of " + warning_type + " - potential security risk", line,
                    "Validate input carefully or consider safer alternatives"));
            }

            return line_errors;
        });
}

std::vector<ShellScriptInterpreter::SyntaxError>
ShellScriptInterpreter::validate_command_existence() {
    std::vector<SyntaxError> errors;

    return errors;
}

std::vector<ShellScriptInterpreter::SyntaxError> ShellScriptInterpreter::analyze_control_flow() {
    std::vector<SyntaxError> errors;

    return errors;
}
