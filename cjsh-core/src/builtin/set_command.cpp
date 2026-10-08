/*
  set_command.cpp

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

#include "set_command.h"

#include "builtin_help.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "error_out.h"
#include "flags.h"
#include "interpreter.h"
#include "numeric_utils.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "string_utils.h"

namespace {

constexpr size_t kOptionNamePadding = 15;

std::string pad_option_name(const std::string& name) {
    if (name.size() >= kOptionNamePadding) {
        return name;
    }
    std::string padded = name;
    (void)padded.append(kOptionNamePadding - name.size(), ' ');
    return padded;
}

void print_option_status(Shell* shell, bool reusable = false) {
    for (const auto& opt : get_shell_option_descriptors()) {
        if (opt.shopt || (config::is_posix_mode() && (opt.option == ShellOption::BraceExpand ||
                                                      opt.option == ShellOption::HistExpand))) {
            continue;
        }
        if (reusable) {
            std::cout << "set " << (shell->get_shell_option(opt.option) ? "-o " : "+o ") << opt.name
                      << '\n';
            continue;
        }
        std::cout << pad_option_name(opt.name) << '\t'
                  << (shell->get_shell_option(opt.option) ? "on" : "off") << '\n';
    }
    if (!config::is_posix_mode() && !reusable) {
        std::cout << pad_option_name("errexit_severity") << '\t' << shell->get_errexit_severity()
                  << '\n';
    }
}

bool apply_short_flag(char flag, bool enable, Shell* shell) {
    auto option = parse_shell_option_short(flag);
    if (!option.has_value()) {
        return false;
    }
    if (config::is_posix_mode() &&
        (*option == ShellOption::BraceExpand || *option == ShellOption::HistExpand)) {
        return false;
    }

    shell->set_shell_option(*option, enable);
    return true;
}

std::string normalize_option_key(std::string key) {
    if (key.empty()) {
        return key;
    }

    size_t first_non_dash = key.find_first_not_of('-');
    if (first_non_dash == std::string::npos) {
        key.clear();
    } else if (first_non_dash > 0) {
        (void)key.erase(0, first_non_dash);
    }

    key = string_utils::to_lower_copy(key);
    std::replace(key.begin(), key.end(), '-', '_');
    return key;
}

bool handle_named_option(const std::string& raw_option, bool enable, Shell* shell) {
    size_t eq_pos = raw_option.find('=');
    std::string option_key =
        (eq_pos == std::string::npos) ? raw_option : raw_option.substr(0, eq_pos);
    std::string option_value = (eq_pos == std::string::npos || eq_pos + 1 >= raw_option.size())
                                   ? ""
                                   : raw_option.substr(eq_pos + 1);

    std::string normalized_key = normalize_option_key(option_key);
    if (normalized_key.empty()) {
        return false;
    }

    if (normalized_key == "errexit_severity") {
        shell->set_errexit_severity(option_value);
        return true;
    }

    if (eq_pos != std::string::npos) {
        return false;
    }

    auto option = parse_shell_option(normalized_key);
    if (option.has_value()) {
        shell->set_shell_option(*option, enable);
        return true;
    }

    return false;
}

bool handle_long_errexit_severity(const std::vector<std::string>& args, size_t& index,
                                  Shell* shell) {
    const std::string& arg = args[index];
    const std::string hyphenated_prefix = "--errexit-severity";
    const std::string underscored_prefix = "--errexit_severity";

    auto matches_prefix = [&](const std::string& prefix) {
        if (arg.rfind(prefix, 0) != 0) {
            return false;
        }

        std::string value;
        if (arg.size() > prefix.size() && arg[prefix.size()] == '=') {
            value = arg.substr(prefix.size() + 1);
        } else if (arg.size() == prefix.size()) {
            if (index + 1 < args.size()) {
                value = args[++index];
            } else {
                value.clear();
            }
        } else {
            return false;
        }

        shell->set_errexit_severity(value);
        return true;
    };

    return matches_prefix(hyphenated_prefix) || matches_prefix(underscored_prefix);
}

void report_invalid_option(const std::string& context) {
    print_error({ErrorType::INVALID_ARGUMENT, "set", "option '" + context + "' not supported", {}});
}

bool option_is_non_posix(ShellOption option) {
    return option == ShellOption::BraceExpand || option == ShellOption::HistExpand;
}

}  // namespace

int set_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(
            args, {"Usage: set [-+eCunxvfnam] [-o option] [--] [ARG ...]",
                   "Set or unset shell options and positional parameters.",
                   "",
                   "Options:",
                   "  -e              Exit on error (errexit)",
                   "  -C              Prevent file overwriting (noclobber)",
                   "  -u              Treat unset variables as error (nounset)",
                   "  -x              Print commands before execution (xtrace)",
                   "  -v              Print input lines as they are read (verbose)",
                   "  -n              Read but don't execute commands (noexec)",
                   "  -f              Disable pathname expansion (noglob)",
                   "  -a              Auto-export modified variables (allexport)",
                   "  -m              Enable job-control monitor mode",
                   "  -o option       Set option by name (pipefail, noclobber, etc.)",
                   "                  Use shopt for globstar, huponexit, and extglob",
                   "                  pipefail makes pipelines return the last non-zero status",
                   "  +<option>       Unset the specified option",
                   "  --              End options; remaining args set $1, $2, etc.",
                   "",
                   "With no arguments, print all environment variables.",
                   "Use 'set -o' to list current option settings.",
                   "",
                   "Special options:",
                   "  --errexit-severity=LEVEL  Set errexit sensitivity level"})) {
        return 0;
    }
    if (shell == nullptr) {
        print_error({ErrorType::FATAL_ERROR, "set", "shell not initialized properly", {}});
        return 1;
    }

    if (args.size() == 1) {
        if (config::is_posix_mode()) {
            auto& manager = shell->get_interpreter()->get_variable_manager();
            auto names = manager.get_variable_names();
            std::sort(names.begin(), names.end());
            for (const auto& name : names) {
                if (!cjsh_env::is_valid_env_name(name)) {
                    continue;
                }
                std::cout << name << '='
                          << cjsh_env::quote_shell_value(manager.get_variable_value(name)) << '\n';
            }
            return 0;
        }
        extern char** environ;
        for (char** env = environ; *env != nullptr; ++env) {
            std::cout << *env << '\n';
        }
        return 0;
    }

    bool parsing_options = true;
    bool positional_specified = false;
    std::vector<std::string> positional_params;
    positional_params.reserve(args.size());

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (parsing_options) {
            if (arg == "--") {
                parsing_options = false;
                positional_specified = true;
                continue;
            }

            if (config::is_posix_mode() && (arg.rfind("--errexit-severity", 0) == 0 ||
                                            arg.rfind("--errexit_severity", 0) == 0)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "set",
                             "errexit severity is disabled in POSIX mode",
                             {}});
                return 2;
            }
            if (!arg.empty() && arg[0] == '-' && handle_long_errexit_severity(args, i, shell)) {
                continue;
            }

            if (arg.rfind("--", 0) == 0) {
                report_invalid_option(arg);
                return 1;
            }

            if (arg.size() > 1 && (arg[0] == '-' || arg[0] == '+')) {
                if (arg[1] == 'o') {
                    bool enable_option = arg[0] == '-';
                    std::string option_name;

                    if (arg.size() == 2) {
                        if (i + 1 >= args.size()) {
                            print_option_status(shell, config::is_posix_mode() && !enable_option);
                            return 0;
                        }
                        option_name = args[++i];
                    } else {
                        option_name = arg.substr(2);
                    }

                    if (option_name.empty()) {
                        report_invalid_option(arg);
                        return 1;
                    }

                    std::string normalized_key = normalize_option_key(option_name);
                    bool inline_value = option_name.find('=') != std::string::npos;

                    if (config::is_posix_mode()) {
                        if (normalized_key.rfind("errexit_severity", 0) == 0) {
                            print_error({ErrorType::INVALID_ARGUMENT,
                                         "set",
                                         "errexit severity is disabled in POSIX mode",
                                         {}});
                            return 2;
                        }
                        auto requested_option = parse_shell_option(normalized_key);
                        if (requested_option.has_value() &&
                            option_is_non_posix(*requested_option)) {
                            print_error(
                                {ErrorType::INVALID_ARGUMENT,
                                 "set",
                                 "option '" + option_name + "' is not available in POSIX mode",
                                 {"Use POSIX options only or run without --posix"}});
                            return 1;
                        }
                    }

                    if (normalized_key == "errexit_severity" && !inline_value) {
                        std::string severity_value;
                        if (i + 1 < args.size()) {
                            severity_value = args[++i];
                        }
                        shell->set_errexit_severity(severity_value);
                        continue;
                    }

                    if (!handle_named_option(option_name, enable_option, shell)) {
                        report_invalid_option(option_name);
                        return 1;
                    }
                    continue;
                } else {
                    std::string flags = arg.substr(1);
                    bool enable_flag = arg[0] == '-';
                    bool ok = true;
                    for (char flag : flags) {
                        if (!apply_short_flag(flag, enable_flag, shell)) {
                            report_invalid_option(std::string(1, arg[0]) + flag);
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) {
                        return 1;
                    }
                    continue;
                }
            }
        }

        parsing_options = false;
        positional_specified = true;
        positional_params.push_back(arg);
    }

    if (positional_specified) {
        flags::set_positional_parameters(positional_params);
    }

    return 0;
}

int shift_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(
            args, {"Usage: shift [N]", "Discard the first N positional parameters (default 1)."})) {
        return 0;
    }
    if (shell == nullptr) {
        print_error({ErrorType::FATAL_ERROR, "shift", "shell not initialized properly", {}});
        return 1;
    }

    if (args.size() > 2) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "shift", "too many arguments", {"Usage: shift [N]"}});
        return 2;
    }

    int shift_count = 1;

    if (args.size() > 1) {
        if (!numeric_utils::parse_int_strict(args[1], shift_count)) {
            print_error(
                {ErrorType::INVALID_ARGUMENT, "shift", "invalid shift count: " + args[1], {}});
            return 1;
        }
        if (shift_count < 0) {
            print_error({ErrorType::INVALID_ARGUMENT, "shift", "negative shift count", {}});
            return 1;
        }
    }

    size_t param_count = flags::get_positional_parameter_count();
    if (shift_count > static_cast<int>(param_count)) {
        print_error({ErrorType::INVALID_ARGUMENT, "shift", "shift count out of range", {}});
        return 1;
    }

    return flags::shift_positional_parameters(shift_count);
}
