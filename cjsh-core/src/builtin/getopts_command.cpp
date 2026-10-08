/*
  getopts_command.cpp

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

#include "getopts_command.h"

#include "builtin_help.h"

#include <cstdlib>
#include <string>
#include <vector>
#include "error_out.h"
#include "flags.h"
#include "interpreter.h"
#include "shell.h"
#include "shell_env.h"

namespace {

void set_special_var(Shell* shell, const std::string& key, const std::string& value) {
    (void)cjsh_env::set_shell_or_local_variable_value(shell, key, value);
}

void set_special_var(Shell* shell, const std::string& key, int value) {
    set_special_var(shell, key, std::to_string(value));
}

void unset_special_var(Shell* shell, const std::string& key) {
    (void)cjsh_env::unset_shell_or_local_variable_value(shell, key);
}

void set_getopts_pos(int value) {
    (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", std::to_string(value));
}

}  // namespace

int getopts_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args,
                            {"Usage: getopts OPTSTRING NAME [ARG ...]",
                             "Parse positional parameters as options, storing results in NAME.",
                             "With no ARG values, uses the shell's positional parameters."})) {
        return 0;
    }
    if (!shell) {
        print_error({ErrorType::FATAL_ERROR, "getopts", "shell not initialized properly", {}});
        return 1;
    }

    if (args.size() < 3) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "getopts",
                     "missing OPTSTRING or NAME operand",
                     {"Usage: getopts OPTSTRING NAME [ARG ...]"}});
        return 2;
    }

    const std::string& optstring = args[1];
    const std::string& name = args[2];

    std::vector<std::string> argv_list;
    if (args.size() > 3) {
        for (size_t i = 3; i < args.size(); ++i) {
            argv_list.push_back(args[i]);
        }
    } else {
        argv_list = flags::get_positional_parameters();
    }

    int optind = 1;
    std::string optind_source;
    if (auto* interpreter = shell->get_interpreter()) {
        optind_source = interpreter->get_variable_value("OPTIND");
    }
    if (optind_source.empty() && cjsh_env::shell_variable_is_set("OPTIND")) {
        optind_source = cjsh_env::get_shell_variable_value("OPTIND");
    }
    if (!optind_source.empty()) {
        try {
            optind = std::stoi(optind_source);
        } catch (...) {
            optind = 1;
        }
    }

    if (optind <= 0) {
        optind = 1;
    }

    if (argv_list.empty() || optind > static_cast<int>(argv_list.size())) {
        set_special_var(shell, "OPTIND", optind);
        (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", "1");
        return 1;
    }

    std::string current_arg = argv_list[static_cast<size_t>(optind - 1)];

    if (current_arg.size() < 2 || current_arg[0] != '-') {
        set_special_var(shell, "OPTIND", optind);
        (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", "1");
        return 1;
    }

    if (current_arg == "--") {
        set_special_var(shell, "OPTIND", optind + 1);
        (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", "1");
        return 1;
    }

    static int char_index = 1;
    if (cjsh_env::shell_variable_is_set("GETOPTS_POS")) {
        std::string optarg_env = cjsh_env::get_shell_variable_value("GETOPTS_POS");
        try {
            char_index = std::stoi(optarg_env);
        } catch (...) {
            char_index = 1;
        }
    } else {
        char_index = 1;
    }

    if (char_index >= static_cast<int>(current_arg.size())) {
        optind++;
        char_index = 1;
        set_special_var(shell, "OPTIND", optind);
        (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", "1");
        return getopts_command(args, shell);
    }

    char opt = current_arg[static_cast<size_t>(char_index)];
    char_index++;

    size_t opt_pos = optstring.find(opt);
    if (opt_pos == std::string::npos) {
        set_special_var(shell, name, "?");

        std::string optarg_val(1, opt);
        set_special_var(shell, "OPTARG", optarg_val);

        if (char_index >= static_cast<int>(current_arg.size())) {
            optind++;
            char_index = 1;
        }
        set_special_var(shell, "OPTIND", optind);
        set_getopts_pos(char_index);

        if (!optstring.empty() && optstring[0] == ':') {
            set_special_var(shell, name, "?");
            return 0;
        }

        print_error(
            {ErrorType::INVALID_ARGUMENT, "getopts", std::string("illegal option -- ") + opt, {}});
        return 1;
    }

    std::string opt_val(1, opt);
    set_special_var(shell, name, opt_val);

    if (opt_pos + 1 < optstring.size() && optstring[opt_pos + 1] == ':') {
        std::string optarg;

        if (char_index < static_cast<int>(current_arg.size())) {
            optarg = current_arg.substr(static_cast<size_t>(char_index));
            optind++;
            char_index = 1;
        } else {
            optind++;
            if (optind <= static_cast<int>(argv_list.size())) {
                optarg = argv_list[static_cast<size_t>(optind - 1)];
                optind++;
            } else {
                if (!optstring.empty() && optstring[0] == ':') {
                    set_special_var(shell, name, ":");
                    set_special_var(shell, "OPTARG", opt_val);
                } else {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 "getopts",
                                 std::string("option requires an argument -- ") + opt,
                                 {}});
                    set_special_var(shell, name, "?");
                    set_special_var(shell, "OPTARG", opt_val);
                }
                set_special_var(shell, "OPTIND", optind);
                (void)cjsh_env::set_shell_variable_value("GETOPTS_POS", "1");
                return 0;
            }
            char_index = 1;
        }

        set_special_var(shell, "OPTARG", optarg);
    } else {
        unset_special_var(shell, "OPTARG");

        if (char_index >= static_cast<int>(current_arg.size())) {
            optind++;
            char_index = 1;
        }
    }

    set_special_var(shell, "OPTIND", optind);
    set_getopts_pos(char_index);

    return 0;
}
