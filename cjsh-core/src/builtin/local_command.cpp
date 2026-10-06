/*
  local_command.cpp

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

#include "local_command.h"
#include <cstddef>
#include <string>
#include <vector>

#include "builtin_help.h"
#include "declare_command.h"

#include "error_out.h"
#include "interpreter.h"
#include "parser_utils.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"

int local_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args,
                            {"Usage: local [-aAnrx] NAME[=VALUE] ...",
                             "Define local variables within a function scope.",
                             "Attributes: -a indexed array, -A associative array, -n nameref,",
                             "            -r readonly, -x exported."})) {
        return 0;
    }

    if (config::is_posix_mode()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "local",
                     "'local' is disabled in POSIX mode",
                     {"Declare variables without 'local'"}});
        return 1;
    }

    auto* script_interpreter = shell->get_shell_script_interpreter();
    if (script_interpreter == nullptr || !script_interpreter->in_function_scope()) {
        print_error({ErrorType::RUNTIME_ERROR, "local", "not available outside of functions", {}});
        return 1;
    }

    if (args.size() == 1) {
        return 0;
    }

    if (args[1].size() > 1 && args[1].front() == '-' &&
        args[1].find_first_not_of("-aAnrx") == std::string::npos) {
        std::vector<std::string> declare_args = args;
        declare_args[0] = "declare";
        return declare_command(declare_args, shell);
    }

    bool all_successful = true;

    for (size_t i = 1; i < args.size();) {
        const std::string& arg = args[i];

        AssignmentOperand operand;
        (void)parse_assignment_operand(arg, operand, false);

        if (operand.name.empty()) {
            print_error({ErrorType::INVALID_ARGUMENT, "local", "invalid variable name", {}});
            all_successful = false;
            continue;
        }

        if (operand.has_assignment && operand.value.empty() && i + 1 < args.size() &&
            args[i + 1] == "(") {
            bool append = false;
            std::string target_name =
                normalize_assignment_target(trim_whitespace(operand.name), append);

            if (!is_valid_identifier(target_name)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "local",
                             "invalid variable name: " + target_name,
                             {}});
                all_successful = false;
                ++i;
                continue;
            }

            size_t close_index = find_closing_parenthesis_token(args, i + 1);

            if (close_index == std::string::npos) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "local",
                             "unclosed array assignment for " + target_name,
                             {}});
                all_successful = false;
                ++i;
                continue;
            }

            if (!script_interpreter->is_local_variable(target_name)) {
                script_interpreter->set_local_variable(target_name, "");
            }

            std::vector<std::string> words(args.begin() + (i + 2), args.begin() + close_index);
            if (!script_interpreter->get_variable_manager().assign_array_literal(target_name, words,
                                                                                 append)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "local",
                             "invalid array assignment for " + target_name,
                             {}});
                all_successful = false;
            }

            i = close_index + 1;
            continue;
        }

        if (operand.has_assignment) {
            bool append = false;
            std::string target_name =
                normalize_assignment_target(trim_whitespace(operand.name), append);

            std::string base_name = assignment_target_base_name(target_name);
            if (!is_valid_identifier(base_name)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "local",
                             "invalid variable name: " + target_name,
                             {}});
                all_successful = false;
                ++i;
                continue;
            }

            if (append) {
                if (!script_interpreter->is_local_variable(base_name)) {
                    script_interpreter->set_local_variable(base_name, "");
                }
                std::string current_value = script_interpreter->get_variable_value(target_name);
                script_interpreter->set_local_variable(target_name, current_value + operand.value);
            } else {
                script_interpreter->set_local_variable(target_name, operand.value);
            }

            ++i;
            continue;
        }

        std::string base_name = assignment_target_base_name(operand.name);
        if (!is_valid_identifier(base_name)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "local",
                         "invalid variable name: " + operand.name,
                         {}});
            all_successful = false;
            ++i;
            continue;
        }

        script_interpreter->set_local_variable(base_name, "");
        ++i;
    }

    return all_successful ? 0 : 1;
}
