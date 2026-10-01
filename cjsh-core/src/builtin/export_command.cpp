/*
  export_command.cpp

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

#include "export_command.h"
#include <stdlib.h>

#include "builtin_help.h"
#include "builtin_option_parser.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "error_out.h"
#include "interpreter.h"
#include "parameter_utils.h"
#include "parser_utils.h"
#include "readonly_command.h"
#include "shell.h"
#include "shell_env.h"

namespace {

bool validate_export_name(const std::string& name, bool& is_readonly) {
    is_readonly = false;
    if (name.empty()) {
        return false;
    }

    if (name[0] == '$') {
        if (parameter_utils::is_special_parameter_reference(name)) {
            is_readonly = true;
        }
        return false;
    }

    if (!is_valid_identifier(name)) {
        return false;
    }

    if (readonly_manager_is(name)) {
        is_readonly = true;
        return false;
    }

    return true;
}

bool parse_unset_target(const std::string& text, std::string& base_name, bool& has_index) {
    has_index = false;
    base_name.clear();

    if (text.empty()) {
        return false;
    }

    size_t left_bracket = text.find('[');
    if (left_bracket == std::string::npos) {
        if (!is_valid_identifier(text)) {
            return false;
        }
        base_name = text;
        return true;
    }

    if (text.back() != ']') {
        return false;
    }

    std::string name = text.substr(0, left_bracket);
    if (!is_valid_identifier(name)) {
        return false;
    }

    std::string index = text.substr(left_bracket + 1, text.length() - left_bracket - 2);
    if (index.empty()) {
        return false;
    }

    base_name = std::move(name);
    has_index = true;
    return true;
}

}  // namespace

int export_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args, {"Usage: export [-p] [NAME[=VALUE] ...]",
                                   "Set environment variables for the shell and subprocesses.",
                                   "Without operands, list exported variables.",
                                   "-p prints exported variables in reusable form."})) {
        return 0;
    }
    size_t start_index = 1;
    const bool options_ok = builtin_parse_short_options(args, start_index, "export",
                                                        [&](char option) { return option == 'p'; });
    if (!options_ok) {
        return 2;
    }

    if (start_index >= args.size()) {
        if (config::is_posix_mode()) {
            for (const auto& name : cjsh_env::exported_variable_names()) {
                if (!cjsh_env::is_valid_env_name(name)) {
                    continue;
                }
                std::cout << "export " << name;
                if (cjsh_env::shell_variable_is_set(name)) {
                    std::cout << '='
                              << cjsh_env::quote_shell_value(
                                     cjsh_env::get_shell_variable_value(name));
                }
                std::cout << '\n';
            }
            return 0;
        }
        extern char** environ;
        for (char** env = environ; *env != nullptr; ++env) {
            std::cout << "export " << *env << '\n';
        }
        return 0;
    }

    bool all_successful = true;
    auto& env_vars = cjsh_env::env_vars();

    for (size_t i = start_index; i < args.size(); ++i) {
        AssignmentOperand operand;
        (void)parse_assignment_operand(args[i], operand, true);

        const std::string& name = operand.name;
        bool is_readonly = false;
        if (!validate_export_name(name, is_readonly)) {
            const std::string message =
                is_readonly ? name + ": readonly variable" : name + ": invalid identifier";
            print_error({ErrorType::INVALID_ARGUMENT, "export", message, {}});
            all_successful = false;
            continue;
        }

        if (config::is_posix_mode()) {
            cjsh_env::mark_exported(name);
            if (!operand.has_assignment && !cjsh_env::shell_variable_is_set(name)) {
                continue;
            }
        }
        if (operand.has_assignment) {
            std::string value = operand.value;

            if (shell != nullptr) {
                if (auto* parser = shell->get_parser()) {
                    parser->expand_env_vars(value);
                }
            }

            env_vars[name] = value;

            (void)setenv(name.c_str(), value.c_str(), 1);
        } else {
            std::string var_value;
            bool is_local = false;

            auto* script_interpreter = shell->get_shell_script_interpreter();
            if (script_interpreter != nullptr && script_interpreter->is_local_variable(name)) {
                var_value = script_interpreter->get_variable_value(name);
                is_local = true;

                script_interpreter->mark_local_as_exported(name);
            } else {
                if (cjsh_env::shell_variable_is_set(name)) {
                    var_value = cjsh_env::get_shell_variable_value(name);
                } else {
                    auto it = env_vars.find(name);
                    if (it != env_vars.end()) {
                        var_value = it->second;
                    } else {
                        var_value.clear();
                    }
                }
            }

            if (!is_local) {
                env_vars[name] = var_value;
            }
            (void)setenv(name.c_str(), var_value.c_str(), 1);
        }
        cjsh_env::sync_parser_env_var(shell, name);
    }

    return all_successful ? 0 : 1;
}

int unset_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args, {"Usage: unset [-n|-v|-f] NAME [NAME ...]",
                                   "Remove variables from the environment and shell state.",
                                   "-n unsets the nameref attribute; -v selects variables."})) {
        return 0;
    }
    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "unset", "not enough arguments", {}});
        return 1;
    }

    bool success = true;
    auto& env_vars = cjsh_env::env_vars();
    auto* script_interpreter = shell->get_shell_script_interpreter();
    bool nameref_only = false;
    bool functions_only = false;
    size_t operand_start = 1;
    if (args.size() > 1 && args[1] == "-n") {
        nameref_only = true;
        operand_start = 2;
    } else if (args.size() > 1 && (args[1] == "-v" || args[1] == "--")) {
        operand_start = 2;
    } else if (args.size() > 1 && args[1] == "-f") {
        functions_only = true;
        operand_start = 2;
    }
    if (operand_start < args.size() && args[operand_start] == "--") {
        ++operand_start;
    }

    for (size_t i = operand_start; i < args.size(); ++i) {
        const std::string& name = args[i];
        if (functions_only) {
            if (!cjsh_env::is_valid_env_name(name)) {
                print_error({ErrorType::INVALID_ARGUMENT, "unset", "invalid name: " + name, {}});
                success = false;
            } else if (script_interpreter && !script_interpreter->unset_function(name)) {
                success = false;
            }
            continue;
        }
        std::string base_name;
        bool has_index = false;

        if (!parse_unset_target(name, base_name, has_index)) {
            print_error({ErrorType::INVALID_ARGUMENT, "unset", "invalid name: " + name, {}});
            success = false;
            continue;
        }

        if (!readonly_manager_can_assign(base_name, "unset")) {
            success = false;
            continue;
        }

        if (script_interpreter != nullptr) {
            if (nameref_only) {
                (void)script_interpreter->get_variable_manager().unset_nameref(base_name);
                continue;
            }
            bool removed = script_interpreter->get_variable_manager().unset_variable(name);
            if (removed || has_index) {
                continue;
            }
        }

        if (has_index) {
            continue;
        }

        (void)env_vars.erase(base_name);

        if (unsetenv(base_name.c_str()) != 0) {
            print_error({ErrorType::RUNTIME_ERROR,
                         "unset",
                         std::string("error unsetting ") + base_name + ": " + strerror(errno),
                         {}});
            success = false;
        }
        cjsh_env::sync_parser_env_var(shell, base_name);
    }

    return success ? 0 : 1;
}
