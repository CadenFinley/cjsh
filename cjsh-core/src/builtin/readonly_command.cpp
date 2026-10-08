/*
  readonly_command.cpp

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

#include "readonly_command.h"

#include "builtin_help.h"
#include "builtin_option_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#include "error_out.h"
#include "interpreter.h"
#include "parameter_utils.h"
#include "parser_utils.h"
#include "shell.h"
#include "shell_env.h"

namespace {
struct ReadonlyState {
    std::unordered_set<std::string> readonly_vars;
};

struct ReadonlyFunctionState {
    std::unordered_set<std::string> readonly_functions;
};

bool is_builtin_readonly_var(const std::string& name) {
    if (parameter_utils::is_special_parameter_name(name)) {
        return true;
    }

    static const std::unordered_set<std::string> kBuiltinReadonlyVars = {
        "PIPESTATUS", "CJSH_VERSION", "EXIT_CODE"};
    return kBuiltinReadonlyVars.find(name) != kBuiltinReadonlyVars.end();
}

ReadonlyState& readonly_state() {
    static ReadonlyState state;
    return state;
}

ReadonlyFunctionState& readonly_function_state() {
    static ReadonlyFunctionState state;
    return state;
}
}  // namespace

void readonly_manager_set(const std::string& name) {
    (void)readonly_state().readonly_vars.insert(name);
}

bool readonly_manager_is(const std::string& name) {
    if (is_builtin_readonly_var(name)) {
        return true;
    }
    const auto& vars = readonly_state().readonly_vars;
    return vars.find(name) != vars.end();
}

bool readonly_manager_can_assign(const std::string& name, const std::string& command_name) {
    if (!readonly_manager_is(name)) {
        return true;
    }

    print_error({ErrorType::INVALID_ARGUMENT, command_name, name + ": readonly variable", {}});
    return false;
}

std::vector<std::string> readonly_manager_list() {
    const auto& vars = readonly_state().readonly_vars;
    std::vector<std::string> result(vars.begin(), vars.end());
    std::sort(result.begin(), result.end());
    return result;
}

void readonly_function_manager_set(const std::string& name) {
    (void)readonly_function_state().readonly_functions.insert(name);
}

bool readonly_function_manager_is(const std::string& name) {
    const auto& funcs = readonly_function_state().readonly_functions;
    return funcs.find(name) != funcs.end();
}

std::vector<std::string> readonly_function_manager_list() {
    const auto& funcs = readonly_function_state().readonly_functions;
    std::vector<std::string> result(funcs.begin(), funcs.end());
    std::sort(result.begin(), result.end());
    return result;
}

void print_readonly_variables(bool quote_values) {
    auto readonly_vars = readonly_manager_list();

    for (const std::string& var : readonly_vars) {
        if (!cjsh_env::shell_variable_is_set(var)) {
            std::cout << "readonly " << var << '\n';
            continue;
        }

        if (quote_values) {
            std::cout << "readonly " << var << '='
                      << cjsh_env::quote_shell_value(cjsh_env::get_shell_variable_value(var))
                      << '\n';
        } else {
            std::cout << "readonly " << var << "=" << cjsh_env::get_shell_variable_value(var)
                      << '\n';
        }
    }
}

int readonly_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(args, {"Usage: readonly [-pf] NAME[=VALUE] ...",
                                   "Mark shell variables as readonly and optionally assign values.",
                                   "-p prints readonly variables.",
                                   "-f operates on shell functions instead of variables."})) {
        return 0;
    }
    if (args.size() == 1) {
        print_readonly_variables(false);
        return 0;
    }

    bool print_mode = false;
    bool function_mode = false;
    size_t start_index = 1;

    const bool options_ok =
        builtin_parse_short_options(args, start_index, "readonly", [&](char option) {
            switch (option) {
                case 'p':
                    print_mode = true;
                    return true;
                case 'f':
                    function_mode = true;
                    return true;
                default:
                    return false;
            }
        });
    if (!options_ok) {
        return 2;
    }

    if (function_mode) {
        if (print_mode || start_index >= args.size()) {
            auto readonly_funcs = readonly_function_manager_list();
            for (const std::string& func : readonly_funcs) {
                std::cout << "readonly -f " << func << '\n';
            }
            return 0;
        }

        if (!shell || !shell->get_interpreter()) {
            print_error(
                {ErrorType::RUNTIME_ERROR, "readonly", "shell interpreter not available", {}});
            return 1;
        }

        auto* interpreter = shell->get_interpreter();
        for (size_t i = start_index; i < args.size(); ++i) {
            const std::string& func_name = args[i];
            if (!interpreter->has_function(func_name)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "readonly",
                             func_name + ": function not found",
                             {}});
                return 1;
            }
            readonly_function_manager_set(func_name);
        }
        return 0;
    }

    if (print_mode) {
        print_readonly_variables(true);
        return 0;
    }

    for (size_t i = start_index; i < args.size(); ++i) {
        AssignmentOperand operand;
        (void)parse_assignment_operand(args[i], operand, false);

        if (operand.has_assignment) {
            if (!cjsh_env::is_valid_env_name(operand.name)) {
                print_error(
                    {ErrorType::INVALID_ARGUMENT, "readonly", "invalid name: " + operand.name, {}});
                return 1;
            }

            if (!readonly_manager_can_assign(operand.name, "readonly")) {
                return 1;
            }

            if (!cjsh_env::set_shell_variable_value(operand.name, operand.value)) {
                print_error(
                    {ErrorType::FATAL_ERROR, "readonly", "shell not initialized properly", {}});
                return 1;
            }

            readonly_manager_set(operand.name);
        } else {
            if (!cjsh_env::is_valid_env_name(operand.name)) {
                print_error(
                    {ErrorType::INVALID_ARGUMENT, "readonly", "invalid name: " + operand.name, {}});
                return 1;
            }
            if ((!cjsh_env::shell_variable_is_set(operand.name)) &&
                (!cjsh_env::set_shell_variable_value(operand.name, ""))) {
                print_error(
                    {ErrorType::FATAL_ERROR, "readonly", "shell not initialized properly", {}});
                return 1;
            }

            readonly_manager_set(operand.name);
        }
    }

    return 0;
}
