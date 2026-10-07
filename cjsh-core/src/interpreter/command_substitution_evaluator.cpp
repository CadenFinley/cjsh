/*
  command_substitution_evaluator.cpp

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

#include "command_substitution_evaluator.h"

#include "exec.h"
#include "function_ref.h"
#include "parser_utils.h"
#include "shell.h"
#include "string_utils.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

namespace {

std::pair<std::string, int> execute_command_for_substitution(
    const std::string& command, const std::function<int(const std::string&)>& executor) {
    auto output = exec_utils::execute_with_stdout_capture(
        [&]() -> int {
            (void)std::cout.flush();
            (void)fflush(nullptr);

            int exit_code = read_exit_code_or(executor(command));
            (void)std::cout.flush();
            (void)std::cerr.flush();
            (void)std::clog.flush();
            (void)fflush(nullptr);
            return exit_code;
        },
        false, false);

    std::string result = string_utils::trim_trailing_line_endings_copy(std::move(output.output));
    if (output.exit_code < 0) {
        return {"", 1};
    }
    return {result, output.exit_code};
}

}  // namespace

CommandSubstitutionEvaluator::CommandSubstitutionEvaluator(CommandExecutor executor)
    : command_executor_(std::move(executor)) {
}

std::pair<std::string, int> CommandSubstitutionEvaluator::capture_command_output(
    const std::string& command) {
    return command_executor_(command);
}

CommandSubstitutionEvaluator::CommandExecutor CommandSubstitutionEvaluator::create_command_executor(
    cjsh::FunctionRef<int(const std::string&)> executor) {
    return [executor](const std::string& command) -> std::pair<std::string, int> {
        return execute_command_for_substitution(command, executor);
    };
}

bool CommandSubstitutionEvaluator::find_matching_delimiter(const std::string& text, size_t start,
                                                           char open_c, char close_c,
                                                           size_t& end_out) {
    int depth = 1;
    bool local_in_q = false;
    char local_q = '\0';

    auto is_escaped = [&](size_t position) {
        size_t slash_count = 0;
        while (position > start && text[position - 1] == '\\') {
            ++slash_count;
            --position;
        }
        return (slash_count % 2) == 1;
    };

    for (size_t j = start; j < text.size(); ++j) {
        char d = text[j];

        if (!local_in_q && (d == '"' || d == '\'') && !is_escaped(j)) {
            local_in_q = true;
            local_q = d;
            continue;
        }

        if (local_in_q && d == local_q && (local_q == '\'' || !is_escaped(j))) {
            local_in_q = false;
            local_q = '\0';
            continue;
        }

        if (!local_in_q) {
            if (d == open_c) {
                depth++;
            } else if (d == close_c) {
                depth--;
                if (depth == 0) {
                    end_out = j;
                    return true;
                }
            }
        }
    }
    return false;
}

bool CommandSubstitutionEvaluator::try_handle_arithmetic_expansion(const std::string& input,
                                                                   size_t& i,
                                                                   std::string& output_text) {
    char c = input[i];
    if (c != '$' || i + 2 >= input.size() || input[i + 1] != '(' || input[i + 2] != '(') {
        return false;
    }

    size_t arith_end = 0;
    if (!find_matching_delimiter(input, i + 3, '(', ')', arith_end)) {
        return false;
    }

    if (arith_end + 1 >= input.size() || input[arith_end + 1] != ')') {
        return false;
    }

    std::string inner = input.substr(i + 3, arith_end - (i + 3));
    if (inner.find(';') != std::string::npos) {
        return false;
    }

    ExpansionResult inner_result;
    inner_result.text = "";
    for (size_t k = 0; k < inner.size(); ++k) {
        char inner_c = inner[k];
        bool handled = false;

        if (inner_c == '$' && k + 1 < inner.size() && inner[k + 1] == '(') {
            if (k + 2 < inner.size() && inner[k + 2] == '(') {
                inner_result.text += inner_c;
            } else {
                size_t cmd_end_pos = 0;
                if (find_matching_delimiter(inner, k + 2, '(', ')', cmd_end_pos)) {
                    std::string cmd_content = inner.substr(k + 2, cmd_end_pos - (k + 2));
                    auto [cmd_output, exit_code] = capture_command_output(cmd_content);
                    inner_result.outputs.push_back(cmd_output);
                    inner_result.exit_codes.push_back(exit_code);

                    std::string trimmed_output =
                        string_utils::trim_trailing_line_endings_copy(std::move(cmd_output));
                    inner_result.text += trimmed_output;
                    k = cmd_end_pos;
                    handled = true;
                }
            }
        }

        if (!handled) {
            inner_result.text += inner_c;
        }
    }

    output_text += "$((";
    output_text += inner_result.text;
    output_text += "))";
    i = arith_end + 1;
    return true;
}

bool CommandSubstitutionEvaluator::try_handle_command_substitution(const std::string& input,
                                                                   size_t& i,
                                                                   ExpansionResult& result,
                                                                   bool in_double_quotes) {
    char c = input[i];
    if (c != '$' || i + 1 >= input.size() || input[i + 1] != '(') {
        return false;
    }

    size_t cmd_end = 0;
    if (!parser_find_matching_command_substitution_end(input, i + 2, cmd_end)) {
        return false;
    }

    std::string cmd_content = input.substr(i + 2, cmd_end - i - 2);
    auto [cmd_output, exit_code] = capture_command_output(cmd_content);
    result.outputs.push_back(cmd_output);
    result.exit_codes.push_back(exit_code);
    append_substitution_result(cmd_output, in_double_quotes, result.text);
    i = cmd_end;
    return true;
}

bool CommandSubstitutionEvaluator::try_handle_backtick_substitution(const std::string& input,
                                                                    size_t& i,
                                                                    ExpansionResult& result,
                                                                    bool in_double_quotes) {
    if (input[i] != '`') {
        return false;
    }

    size_t backtick_end = find_closing_backtick(input, i + 1);
    if (backtick_end == std::string::npos) {
        return false;
    }

    std::string cmd_content = input.substr(i + 1, backtick_end - i - 1);
    auto [cmd_output, exit_code] = capture_command_output(cmd_content);
    result.outputs.push_back(cmd_output);
    result.exit_codes.push_back(exit_code);
    append_substitution_result(cmd_output, in_double_quotes, result.text);
    i = backtick_end;
    return true;
}

bool CommandSubstitutionEvaluator::try_handle_parameter_expansion(const std::string& input,
                                                                  size_t& i,
                                                                  std::string& output_text) {
    char c = input[i];
    if (c != '$' || i + 1 >= input.size() || input[i + 1] != '{') {
        return false;
    }

    size_t brace_end = 0;
    if (!find_matching_delimiter(input, i + 2, '{', '}', brace_end)) {
        return false;
    }

    for (size_t k = i; k <= brace_end; ++k) {
        output_text += input[k];
    }
    i = brace_end;
    return true;
}

size_t CommandSubstitutionEvaluator::find_closing_backtick(const std::string& input, size_t start) {
    bool bt_escaped = false;
    for (size_t pos = start; pos < input.size(); ++pos) {
        if (bt_escaped) {
            bt_escaped = false;
            continue;
        }
        if (input[pos] == '\\') {
            bt_escaped = true;
            continue;
        }
        if (input[pos] == '`') {
            return pos;
        }
    }
    return std::string::npos;
}

void CommandSubstitutionEvaluator::append_substitution_result(const std::string& content,
                                                              bool in_double_quotes,
                                                              std::string& output) {
    if (in_double_quotes) {
        std::string escaped_content;
        escaped_content.reserve(content.size());
        for (char c : content) {
            if (c == '"' || c == '\\') {
                escaped_content += '\\';
            }
            escaped_content += c;
        }

        output += noenv_start();
        output += escaped_content;
        output += noenv_end();
    } else {
        output += subst_literal_start();
        output += noenv_start();
        output += content;
        output += noenv_end();
        output += subst_literal_end();
    }
}

bool CommandSubstitutionEvaluator::handle_escape_sequence(char c, bool& escaped,
                                                          std::string& output) {
    if (escaped) {
        output += '\\';
        output += c;
        escaped = false;
        return true;
    }
    return false;
}

bool CommandSubstitutionEvaluator::handle_quote_toggle(char c, bool, bool& in_quotes,
                                                       char& quote_char, std::string& output) {
    if ((c == '"' || c == '\'') && !in_quotes) {
        in_quotes = true;
        quote_char = c;
        output += c;
        return true;
    }
    if (in_quotes && c == quote_char) {
        in_quotes = false;
        quote_char = '\0';
        output += c;
        return true;
    }
    return false;
}

CommandSubstitutionEvaluator::ExpansionResult CommandSubstitutionEvaluator::expand_substitutions(
    const std::string& input) {
    ExpansionResult result;
    result.text.reserve(input.size());

    bool in_quotes = false;
    char q = '\0';
    bool escaped = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (handle_escape_sequence(c, escaped, result.text)) {
            continue;
        }

        if (c == '\\' && (!in_quotes || q != '\'')) {
            escaped = true;
            continue;
        }

        if (handle_quote_toggle(c, q == '\'', in_quotes, q, result.text)) {
            continue;
        }

        bool can_substitute = !in_quotes || q == '"';
        if (can_substitute) {
            bool in_double_quotes = in_quotes && q == '"';

            if (try_handle_arithmetic_expansion(input, i, result.text)) {
                continue;
            }

            if (try_handle_command_substitution(input, i, result, in_double_quotes)) {
                continue;
            }

            if (try_handle_backtick_substitution(input, i, result, in_double_quotes)) {
                continue;
            }

            if (try_handle_parameter_expansion(input, i, result.text)) {
                continue;
            }
        }

        result.text += c;
    }

    return result;
}
