/*
  command_command.cpp

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

#include "command_command.h"

#include "builtin_help.h"
#include "builtin_option_parser.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "error_out.h"
#include "shell.h"
#include "shell_env.h"

int command_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args, {"Usage: command [-pVv] COMMAND [ARG ...]",
                                   "Execute COMMAND with arguments, bypassing shell functions.", "",
                                   "Options:", "  -p    Use a default PATH value",
                                   "  -v    Print a description of COMMAND (similar to type)",
                                   "  -V    Print a more verbose description of COMMAND"})) {
        return 0;
    }

    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "command",
                     "missing command operand",
                     {"Usage: command [-pVv] COMMAND [ARG ...]"}});
        return 2;
    }

    bool use_default_path = false;
    bool describe_command = false;
    bool verbose_description = false;
    size_t start_index = 1;

    const bool options_ok =
        builtin_parse_short_options(args, start_index, "command", [&](char option) {
            switch (option) {
                case 'p':
                    use_default_path = true;
                    return true;
                case 'v':
                    describe_command = true;
                    return true;
                case 'V':
                    verbose_description = true;
                    return true;
                default:
                    return false;
            }
        });
    if (!options_ok) {
        return 2;
    }

    if (start_index >= args.size()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "command",
                     "missing command operand",
                     {"Usage: command [-pVv] COMMAND [ARG ...]"}});
        return 2;
    }

    const std::string& command_name = args[start_index];

    if (describe_command || verbose_description) {
        if (command_lookup::is_shell_builtin(command_name, shell)) {
            if (verbose_description) {
                std::cout << command_name << " is a shell builtin\n";
            } else {
                std::cout << command_name << "\n";
            }
            return 0;
        }

        std::optional<std::string> saved_path;
        if (use_default_path) {
            if (cjsh_env::shell_variable_is_set("PATH")) {
                saved_path = cjsh_env::get_shell_variable_value("PATH");
            }

            (void)cjsh_env::set_shell_variable_value("PATH", "/usr/bin:/bin");
        }

        const std::string full_path = cjsh_filesystem::find_executable_in_path(command_name);

        if (use_default_path) {
            if (saved_path) {
                (void)cjsh_env::set_shell_variable_value("PATH", *saved_path);
            } else {
                (void)cjsh_env::unset_shell_variable_value("PATH");
            }
        }

        if (!full_path.empty()) {
            if (verbose_description) {
                std::cout << command_name << " is " << full_path << "\n";
            } else {
                std::cout << full_path << "\n";
            }
            return 0;
        }

        if (verbose_description) {
            print_error({ErrorType::UNKNOWN_ERROR,
                         ErrorSeverity::ERROR,
                         "command",
                         command_name + ": not found",
                         {}});
        }
        return 1;
    }

    if (shell == nullptr) {
        print_error({ErrorType::FATAL_ERROR, "command", "shell not initialized properly", {}});
        return 2;
    }

    std::vector<std::string> exec_args(args.begin() + static_cast<long>(start_index), args.end());

    std::optional<std::string> saved_path;
    if (use_default_path) {
        if (cjsh_env::shell_variable_is_set("PATH")) {
            saved_path = cjsh_env::get_shell_variable_value("PATH");
        }

        (void)cjsh_env::set_shell_variable_value("PATH", "/usr/bin:/bin");
    }

    RegularBuiltinScope regular_scope(command_name);
    int exit_code = shell->execute_command(exec_args, false);

    if (use_default_path) {
        if (saved_path) {
            (void)cjsh_env::set_shell_variable_value("PATH", *saved_path);
        } else {
            (void)cjsh_env::unset_shell_variable_value("PATH");
        }
    }

    return exit_code;
}
