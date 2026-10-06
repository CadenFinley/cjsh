/*
  flags.cpp

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

#include "flags.h"

#include <getopt.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "agent_mode.h"
#include "error_out.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "usage.h"

namespace flags {

std::vector<std::string>& startup_args() {
    static std::vector<std::string> args;
    return args;
}

namespace {

constexpr int kOptNoCompletionLearning = 256;
constexpr int kOptNoSmartCd = 257;
constexpr int kOptNoScriptExtensionInterpreter = 258;
constexpr int kOptNoExec = 259;
constexpr int kOptPosix = 260;
constexpr int kOptNoErrorSuggestions = 261;
constexpr int kOptNoPromptVars = 262;
constexpr int kOptNoHistory = 263;
constexpr int kOptNoAgent = 264;
constexpr int kOptNoConfig = 265;
constexpr int kOptConfigDir = 266;
constexpr int kOptNoSystemPaths = 267;
constexpr int kOptDialect = 269;
constexpr int kOptVersion = 270;
constexpr int kOptHelp = 271;
constexpr int kOptNoColors = 272;
constexpr int kOptNoCompletions = 273;
constexpr int kOptMinimal = 274;
constexpr int kOptSecure = 275;
constexpr int kOptNoHistoryExpansion = 276;
std::vector<std::string> positional_parameters;
bool login_shell_invocation = false;
bool sh_invocation = false;

bool invoked_via_sh(const char* arg0) {
    if (arg0 == nullptr) {
        return false;
    }

    std::string_view shell_name(arg0);
    const std::size_t slash_pos = shell_name.find_last_of('/');
    if (slash_pos != std::string_view::npos) {
        shell_name.remove_prefix(slash_pos + 1);
    }
    if (!shell_name.empty() && shell_name.front() == '-') {
        shell_name.remove_prefix(1);
    }

    return shell_name == "sh";
}

void detect_login_mode(char* argv[]) {
    // detect argv[0] being -cjsh
    login_shell_invocation = (argv != nullptr) && (argv[0] != nullptr) && argv[0][0] == '-';
    if (login_shell_invocation) {
        config::login_mode = true;
    }
}

void apply_minimal_mode() {
    // literally disable everything which turns cjsh into a worse bash or zsh or oh my zsh which is
    // pretty bad
    config::minimal_mode = true;
    config::colors_enabled = false;
    config::source_enabled = false;
    config::completions_enabled = false;
    config::completion_learning_enabled = false;
    config::smart_cd_enabled = false;
    config::syntax_highlighting_enabled = false;
    config::show_startup_time = false;
    config::show_title_line = false;
    config::history_expansion_enabled = false;
    config::status_line_enabled = false;
    config::error_suggestions_enabled = false;
    config::prompt_vars_enabled = false;
}

}  // namespace

bool is_login_shell_invocation() {
    return login_shell_invocation;
}

void warn_if_invoked_via_sh() {
    if (sh_invocation && !config::suppress_sh_warning) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     ErrorSeverity::WARNING,
                     "sh",
                     "cjsh was invoked as sh, but it is not 100% POSIX compliant in its "
                     "interactive behaviors",
                     {"Pass --no-sh-warning to hide this warning"}});
    }
}

void save_startup_arguments(int argc, char* argv[]) {
    // Save startup args for restart/prompt helpers that mirror invocation identity.
    auto& args = startup_args();
    args.clear();
    for (int i = 0; i < argc; i++) {
        (void)args.emplace_back(argv[i]);
    }
}

ParseResult parse_arguments(int argc, char* argv[]) {
    ParseResult result;

    detect_login_mode(argv);

    sh_invocation = invoked_via_sh(argc > 0 ? argv[0] : nullptr);
    if (sh_invocation) {
        config::set_shell_dialect(config::ShellDialect::Posix);
    }
    bool read_stdin = false;

    static struct option long_options[] = {
        {"login", no_argument, nullptr, 'l'},
        {"interactive", no_argument, nullptr, 'i'},
        {"command", required_argument, nullptr, 'c'},
        {"no-exec", no_argument, nullptr, kOptNoExec},
        {"no-config", no_argument, nullptr, kOptNoConfig},
        {"config-dir", required_argument, nullptr, kOptConfigDir},
        {"no-system-paths", no_argument, nullptr, kOptNoSystemPaths},
        {"posix", no_argument, nullptr, kOptPosix},
        {"dialect", required_argument, nullptr, kOptDialect},
        {"version", no_argument, nullptr, kOptVersion},
        {"help", no_argument, nullptr, kOptHelp},
        {"no-colors", no_argument, nullptr, kOptNoColors},
        {"no-titleline", no_argument, nullptr, 'L'},
        {"show-startup-time", no_argument, nullptr, 'U'},
        {"no-source", no_argument, nullptr, 'N'},
        {"no-completions", no_argument, nullptr, kOptNoCompletions},
        {"no-completion-learning", no_argument, nullptr, kOptNoCompletionLearning},
        {"no-smart-cd", no_argument, nullptr, kOptNoSmartCd},
        {"no-script-extension-interpreter", no_argument, nullptr, kOptNoScriptExtensionInterpreter},
        {"no-syntax-highlighting", no_argument, nullptr, 'S'},
        {"no-error-suggestions", no_argument, nullptr, kOptNoErrorSuggestions},
        {"no-prompt-vars", no_argument, nullptr, kOptNoPromptVars},
        {"no-history", no_argument, nullptr, kOptNoHistory},
        {"no-agent", no_argument, nullptr, kOptNoAgent},
        {"minimal", no_argument, nullptr, kOptMinimal},
        {"secure", no_argument, nullptr, kOptSecure},
        {"no-history-expansion", no_argument, nullptr, kOptNoHistoryExpansion},
        {"no-sh-warning", no_argument, nullptr, 'W'},
        {nullptr, 0, nullptr, 0}};

    const char* short_options = "+:lic:nvhCLUNOSmsHW";

    int option_index = 0;
    optind = 1;
    opterr = 0;

    while (true) {
        if (optind < argc) {
            const std::string arg(argv[optind]);
            if (arg.size() > 1 && (arg[0] == '+' || (arg[0] == '-' && arg[1] != '-'))) {
                ++optind;
                const bool enable = arg[0] == '-';
                bool command_operand = false;
                for (size_t j = 1; j < arg.size(); ++j) {
                    const char flag = arg[j];
                    if (flag == 'c') {
                        command_operand = true;
                    } else if (flag == 'o' || flag == 'O') {
                        std::string operand = arg.substr(j + 1);
                        if (operand.empty() &&
                            (optind >= argc || argv[optind][0] == '-' || argv[optind][0] == '+')) {
                            result.option_queries.emplace_back(flag == 'O', !enable);
                            break;
                        }
                        if (operand.empty() && optind < argc) {
                            operand = argv[optind++];
                        }
                        if (operand.empty() && flag == 'o') {
                            print_error({ErrorType::INVALID_ARGUMENT,
                                         "startup",
                                         "-o requires an option",
                                         {}});
                            result.should_exit = true;
                            result.exit_code = 2;
                            return result;
                        }
                        if (auto option = flag == 'O' ? parse_shopt_option(operand)
                                                      : parse_shell_option(operand);
                            option) {
                            result.shell_options.emplace_back(operand, enable);
                            if (*option == ShellOption::Noexec) {
                                config::no_exec = enable;
                            }
                        } else {
                            print_error({ErrorType::INVALID_ARGUMENT,
                                         "startup",
                                         "invalid shell option: " + operand,
                                         {}});
                            result.should_exit = true;
                            result.exit_code = 2;
                            return result;
                        }
                        break;
                    } else if (flag == 's') {
                        read_stdin = enable;
                    } else if (flag == 'i') {
                        config::force_interactive = enable;
                    } else if (flag == 'l') {
                        config::login_mode = enable;
                    } else if (auto option = parse_shell_option_short(flag)) {
                        for (const auto& descriptor : get_shell_option_descriptors()) {
                            if (descriptor.option == *option) {
                                result.shell_options.emplace_back(descriptor.name, enable);
                            }
                        }
                        if (flag == 'n') {
                            config::no_exec = enable;
                        }
                    } else if (flag == 'L') {
                        config::show_title_line = !enable;
                    } else if (flag == 'U') {
                        config::show_startup_time = enable;
                    } else if (flag == 'N') {
                        config::source_enabled = !enable;
                    } else if (flag == 'S') {
                        config::syntax_highlighting_enabled = !enable;
                    } else if (flag == 'W') {
                        config::suppress_sh_warning = enable;
                    } else {
                        print_error({ErrorType::INVALID_ARGUMENT,
                                     "startup",
                                     "invalid option: -" + std::string(1, flag),
                                     {get_usage()}});
                        result.should_exit = true;
                        result.exit_code =
                            config::shell_dialect() == config::ShellDialect::Cjsh ? 1 : 2;
                        return result;
                    }
                }
                if (command_operand) {
                    if (optind >= argc) {
                        print_error({ErrorType::INVALID_ARGUMENT,
                                     "startup",
                                     "option requires an argument: -c",
                                     {get_usage()}});
                        result.should_exit = true;
                        result.exit_code =
                            config::shell_dialect() == config::ShellDialect::Cjsh ? 1 : 2;
                        return result;
                    }
                    config::execute_command = true;
                    config::cmd_to_execute = argv[optind++];
                    config::interactive_mode = false;
                    config::history_expansion_enabled = false;
                    break;
                }
                continue;
            }
        }
        // getopt may leave optind on the current argument within a short-option bundle.
        const int argument_index = optind;
        const int c = getopt_long(argc, argv, short_options, long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'l':
                config::login_mode = true;
                break;
            case 'i':
                config::force_interactive = true;
                break;
            case 'c':
                config::execute_command = true;
                config::cmd_to_execute = optarg;
                config::interactive_mode = false;
                config::history_expansion_enabled = false;
                break;
            case 'n':
            case kOptNoExec:
                result.shell_options.emplace_back("noexec", true);
                config::no_exec = true;
                break;
            case kOptNoConfig:
                config::no_config = true;
                break;
            case kOptConfigDir:
                if (optarg[0] == '\0') {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 "startup",
                                 "--config-dir requires a nonempty directory",
                                 {get_usage()}});
                    result.exit_code = 1;
                    result.should_exit = true;
                    return result;
                }
                config::config_directory = optarg;
                break;
            case kOptNoSystemPaths:
                config::no_system_paths = true;
                break;
            case kOptPosix:
                config::set_shell_dialect(config::ShellDialect::Posix);
                break;
            case kOptDialect: {
                auto dialect = config::parse_shell_dialect(optarg);
                if (!dialect) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 "startup",
                                 "invalid dialect: " + std::string(optarg),
                                 {"Use cjsh or posix"}});
                    result.should_exit = true;
                    result.exit_code = 2;
                    return result;
                }
                config::set_shell_dialect(*dialect);
                break;
            }
            case kOptVersion:
                config::show_version = true;
                config::interactive_mode = false;
                break;
            case kOptHelp:
                config::show_help = true;
                config::interactive_mode = false;
                break;
            case kOptNoColors:
                config::colors_enabled = false;
                break;
            case 'L':
                config::show_title_line = false;
                break;
            case 'U':
                config::show_startup_time = true;
                break;
            case 'N':
                config::source_enabled = false;
                break;
            case kOptNoCompletions:
                config::completions_enabled = false;
                break;
            case kOptNoCompletionLearning:
                config::completion_learning_enabled = false;
                break;
            case kOptNoSmartCd:
                config::smart_cd_enabled = false;
                break;
            case kOptNoScriptExtensionInterpreter:
                config::script_extension_interpreter_enabled = false;
                break;
            case 'S':
                config::syntax_highlighting_enabled = false;
                break;
            case kOptNoErrorSuggestions:
                config::error_suggestions_enabled = false;
                break;
            case kOptNoPromptVars:
                config::prompt_vars_enabled = false;
                break;
            case kOptNoHistory:
                config::history_enabled = false;
                config::history_expansion_enabled = false;
                break;
            case kOptNoAgent:
                agent_mode::disable_for_startup();
                break;
            case kOptMinimal:
                apply_minimal_mode();
                break;
            case kOptSecure:
                config::secure_mode = true;
                config::smart_cd_enabled = false;
                config::history_enabled = false;
                config::history_expansion_enabled = false;
                break;
            case kOptNoHistoryExpansion:
                config::history_expansion_enabled = false;
                break;
            case 'W':
                config::suppress_sh_warning = true;
                break;
            case ':':
            case '?': {
                std::string option =
                    (argument_index > 0 && argument_index < argc) ? argv[argument_index] : "";
                if (option.rfind("--", 0) != 0 && optopt > 0 && optopt < 256) {
                    option = "-" + std::string(1, static_cast<char>(optopt));
                }
                const std::string message = c == ':' ? "option requires an argument: " + option
                                                     : "invalid option: " + option;
                print_error({ErrorType::INVALID_ARGUMENT, "startup", message, {get_usage()}});
                result.exit_code = 1;
                result.should_exit = true;
                return result;
            }
            default:
                print_error({ErrorType::INVALID_ARGUMENT,
                             std::string(1, static_cast<char>(c)),
                             "Unrecognized option",
                             {"Check command line arguments"}});
                result.exit_code = 1;
                result.should_exit = true;
                return result;
        }
        if (config::execute_command) {
            break;
        }
    }

    config::read_stdin = read_stdin;
    if (read_stdin && !config::execute_command) {
        for (int i = optind; i < argc; ++i) {
            result.script_args.emplace_back(argv[i]);
        }
    } else if (optind < argc) {
        result.script_file = argv[optind];
        config::interactive_mode = false;

        for (int i = optind + 1; i < argc; i++) {
            result.script_args.push_back(argv[i]);
        }
    }

    if (!config::force_interactive && (isatty(STDIN_FILENO) == 0)) {
        config::interactive_mode = false;
        config::history_expansion_enabled = false;
    }

    if (config::force_interactive) {
        config::interactive_mode = true;
    }

    if (config::is_posix_mode()) {
        for (const auto& [shopt, reusable] : result.option_queries) {
            (void)reusable;
            if (shopt) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "startup",
                             "shopt is not available in POSIX mode",
                             {}});
                result.should_exit = true;
                result.exit_code = 2;
                return result;
            }
        }
        for (const auto& [name, enabled] : result.shell_options) {
            (void)enabled;
            if (parse_shopt_option(name) || name == "braceexpand" || name == "histexpand") {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "startup",
                             "option '" + name + "' is not available in POSIX mode",
                             {}});
                result.should_exit = true;
                result.exit_code = 2;
                break;
            }
        }
    }

    return result;
}

void set_positional_parameters(const std::vector<std::string>& params) {
    positional_parameters = params;
}

int shift_positional_parameters(int count) {
    if (count < 0) {
        return 1;
    }

    if (static_cast<size_t>(count) >= positional_parameters.size()) {
        positional_parameters.clear();
    } else {
        (void)positional_parameters.erase(positional_parameters.begin(),
                                          positional_parameters.begin() + count);
    }

    return 0;
}

std::vector<std::string> get_positional_parameters() {
    return positional_parameters;
}

size_t get_positional_parameter_count() {
    return positional_parameters.size();
}

}  // namespace flags
