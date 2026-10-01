/*
  variable_expander.cpp

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

#include "variable_expander.h"

#include <unistd.h>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "cjsh_filesystem.h"
#include "exec.h"
#include "flags.h"
#include "interpreter.h"
#include "parameter_utils.h"
#include "parser.h"
#include "parser_utils.h"
#include "quote_info.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"

namespace {

std::string trim_command_output(std::string value) {
    return string_utils::trim_trailing_line_endings_copy(std::move(value));
}

}  // namespace

VariableExpander::VariableExpander(Shell* shell,
                                   const std::unordered_map<std::string, std::string>& env_vars)
    : shell(shell), env_vars(env_vars) {
}

std::string VariableExpander::get_variable_value(const std::string& var_name) {
    if ((shell != nullptr) && (shell->get_shell_script_interpreter() != nullptr)) {
        std::string result = shell->get_shell_script_interpreter()->get_variable_value(var_name);
        return result;
    }

    return cjsh_env::get_shell_variable_value(var_name);
}

std::string VariableExpander::get_exported_variable_value(const std::string& var_name) {
    if (parameter_utils::is_named_special_parameter_name(var_name) &&
        ((shell != nullptr) && (shell->get_shell_script_interpreter() != nullptr))) {
        return shell->get_shell_script_interpreter()->get_variable_value(var_name);
    }

    if ((var_name.length() == 1 && (isdigit(var_name[0]) != 0)) &&
        ((shell != nullptr) && (shell->get_shell_script_interpreter() != nullptr))) {
        return shell->get_shell_script_interpreter()->get_variable_value(var_name);
    }

    return cjsh_env::get_shell_variable_value(var_name);
}

std::string VariableExpander::resolve_parameter_value(const std::string& var_name) {
    if (var_name.empty()) {
        return "";
    }

    if (var_name != "-") {
        static const std::string cached_pid = std::to_string(getpid());
        std::string special_value =
            parameter_utils::get_special_parameter_value(var_name, cached_pid);
        if (!special_value.empty() || parameter_utils::is_named_special_parameter_name(var_name)) {
            return special_value;
        }
    }

    if (var_name == "-") {
        std::string flags;
        auto append_flag = [&](char flag, bool enabled) {
            if (enabled && flags.find(flag) == std::string::npos) {
                flags.push_back(flag);
            }
        };

        append_flag(
            'h', shell ? shell->get_shell_option(ShellOption::Hashall) : !config::is_posix_mode());
        append_flag('B', shell ? shell->get_shell_option(ShellOption::BraceExpand)
                               : !config::is_posix_mode());

        if (shell != nullptr) {
            append_flag('i', shell->get_interactive_mode());
            append_flag('m', shell->is_job_control_enabled());
            for (const auto& descriptor : get_shell_option_descriptors()) {
                if (descriptor.short_flag == 0) {
                    continue;
                }
                append_flag(descriptor.short_flag, shell->get_shell_option(descriptor.option));
            }
        }
        return flags;
    }

    if (!var_name.empty() && (std::isdigit(static_cast<unsigned char>(var_name[0])) != 0) &&
        var_name.length() == 1) {
        std::string value = get_variable_value(var_name);
        if (!value.empty()) {
            return value;
        }

        auto params = flags::get_positional_parameters();
        int param_num = var_name[0] - '0';
        if (param_num > 0) {
            size_t param_index = static_cast<size_t>(param_num - 1);
            if (param_index < params.size()) {
                return params[param_index];
            }
        }

        auto it = env_vars.find(var_name);
        if (it != env_vars.end()) {
            return it->second;
        }
        if (config::is_posix_mode() && shell && shell->get_shell_option(ShellOption::Nounset)) {
            throw std::runtime_error(var_name + ": parameter not set");
        }
        return "";
    }

    std::string value = get_variable_value(var_name);
    if (value.empty()) {
        bool is_set_in_interpreter = false;
        if ((shell != nullptr) && (shell->get_shell_script_interpreter() != nullptr)) {
            is_set_in_interpreter =
                shell->get_shell_script_interpreter()->get_variable_manager().variable_is_set(
                    var_name);
        }

        auto it = env_vars.find(var_name);
        if (it != env_vars.end()) {
            value = it->second;
        } else if (!is_set_in_interpreter && shell != nullptr &&
                   shell->get_shell_option(ShellOption::Nounset)) {
            std::string error_msg = var_name + ": parameter not set";
            throw std::runtime_error(error_msg);
        }
    }
    return value;
}

void VariableExpander::expand_env_vars(std::string& arg) {
    std::string result;

    result.reserve(arg.length() * 2);
    bool in_var = false;
    std::string var_name;
    var_name.reserve(64);

    for (size_t i = 0; i < arg.length(); ++i) {
        if (try_append_arithmetic_expansion(
                arg, i, result, [this](std::string& s) { expand_env_vars(s); }, false)) {
            continue;
        }

        if (arg[i] == '$' && i > 0 && arg[i - 1] == '\\') {
            if (!result.empty() && result.back() == '\\') {
                result.pop_back();
            }
            result += '$';
            continue;
        }

        if (arg[i] == '$' && i + 1 < arg.length() && arg[i + 1] == '{') {
            size_t start = i + 2;
            size_t end = 0;
            if (parser_find_matching_parameter_expansion_end(arg, i + 1, end)) {
                std::string param_expr = arg.substr(start, end - start);
                std::string value;

                if (!config::is_posix_mode() && (param_expr.find(':') != std::string::npos ||
                                                 param_expr.find('-') != std::string::npos)) {
                    value = expand_parameter_with_default(
                        param_expr,
                        [this](const std::string& name) { return get_variable_value(name); },
                        [this](std::string& val) { expand_env_vars(val); });
                } else {
                    if ((shell != nullptr) && (shell->get_shell_script_interpreter() != nullptr)) {
                        try {
                            value =
                                shell->get_shell_script_interpreter()->expand_parameter_expression(
                                    param_expr);
                        } catch (...) {
                            if (config::is_posix_mode()) {
                                throw;
                            }
                            value = get_variable_value(param_expr);
                        }
                    } else {
                        value = get_variable_value(param_expr);
                    }
                }

                result += value;
                i = end;
                continue;
            }
        }

        if (in_var) {
            if (!var_name.empty() &&
                ((std::isdigit(static_cast<unsigned char>(var_name.front())) != 0) ||
                 parameter_utils::is_special_parameter_char(var_name.front()) || var_name == "-")) {
                in_var = false;
                result += resolve_parameter_value(var_name);
                --i;
                continue;
            }

            if (is_valid_identifier_char(arg[i]) ||
                (var_name.empty() &&
                 (parameter_utils::is_special_parameter_char(arg[i]) || arg[i] == '-'))) {
                var_name += arg[i];
            } else {
                in_var = false;
                std::string value = resolve_parameter_value(var_name);
                result += value;

                if (arg[i] != '$') {
                    result += arg[i];
                } else {
                    i--;
                }
            }
        } else if (arg[i] == '$' && (i + 1 < arg.length()) &&
                   (is_valid_identifier_start(arg[i + 1]) ||
                    (std::isdigit(static_cast<unsigned char>(arg[i + 1])) != 0) ||
                    parameter_utils::is_special_parameter_char(arg[i + 1]) || arg[i + 1] == '-')) {
            in_var = true;
            var_name.clear();
            continue;
        } else {
            result += arg[i];
        }
    }

    if (in_var) {
        result += resolve_parameter_value(var_name);
    }

    arg = result;
}

void VariableExpander::expand_word(std::string& arg, std::vector<bool>& expanded_bytes) {
    std::string result;
    expanded_bytes.clear();
    auto append = [&](const std::string& value, bool expanded) {
        result += value;
        expanded_bytes.insert(expanded_bytes.end(), value.size(), expanded);
    };
    for (size_t i = 0; i < arg.size();) {
        if (arg.compare(i, subst_literal_start().size(), subst_literal_start()) == 0) {
            const size_t begin = i + subst_literal_start().size();
            const size_t end = arg.find(subst_literal_end(), begin);
            if (end != std::string::npos) {
                append(strip_noenv_sentinels(arg.substr(begin, end - begin)).first, true);
                i = end + subst_literal_end().size();
                continue;
            }
        }
        if (arg.compare(i, noenv_start().size(), noenv_start()) == 0) {
            const size_t begin = i + noenv_start().size();
            const size_t end = arg.find(noenv_end(), begin);
            if (end != std::string::npos) {
                append(arg.substr(begin, end - begin), false);
                i = end + noenv_end().size();
                continue;
            }
        }
        if (arg[i] == QUOTE_PREFIX && i + 1 < arg.size()) {
            append(arg.substr(i, 2), false);
            i += 2;
            continue;
        }
        if (arg[i] == '\\' && i + 1 < arg.size() && arg[i + 1] == '$') {
            append("$", false);
            i += 2;
            continue;
        }
        size_t end = i;
        if (arg[i] == '$' && i + 1 < arg.size()) {
            if (arg[i + 1] == '{') {
                if (parser_find_matching_parameter_expansion_end(arg, i + 1, end)) {
                    ++end;
                } else {
                    end = i;
                }
            } else if (arg[i + 1] == '(') {
                size_t begin, close;
                if (!parser_find_balanced_double_parens(arg, i + 1, begin, close, end)) {
                    end = i;
                }
            } else if (std::isdigit(static_cast<unsigned char>(arg[i + 1])) ||
                       parameter_utils::is_special_parameter_char(arg[i + 1]) ||
                       arg[i + 1] == '-') {
                end = i + 2;
            } else if (is_valid_identifier_start(arg[i + 1])) {
                end = i + 2;
                while (end < arg.size() && is_valid_identifier_char(arg[end])) {
                    ++end;
                }
            }
        }
        if (end > i) {
            std::string value = arg.substr(i, end - i);
            expand_env_vars(value);
            append(value, true);
            i = end;
        } else {
            append(arg.substr(i++, 1), false);
        }
    }
    arg = std::move(result);
}

void VariableExpander::expand_env_vars_selective(std::string& arg) {
    const std::string& start_marker = noenv_start();
    const std::string& end_marker = noenv_end();

    std::string result;
    result.reserve(arg.length() + arg.length() / 2);

    size_t pos = 0;
    while (pos < arg.length()) {
        size_t start_pos = arg.find(start_marker, pos);

        if (start_pos == std::string::npos) {
            std::string remaining = arg.substr(pos);
            expand_env_vars(remaining);
            result += remaining;
            break;
        }

        std::string before_marker = arg.substr(pos, start_pos - pos);
        expand_env_vars(before_marker);
        result += before_marker;

        size_t end_pos = arg.find(end_marker, start_pos + start_marker.length());
        if (end_pos == std::string::npos) {
            result += arg.substr(start_pos);
            break;
        }

        size_t content_start = start_pos + start_marker.length();
        size_t content_length = end_pos - content_start;
        result += arg.substr(content_start, content_length);

        pos = end_pos + end_marker.length();
    }

    arg = result;
}

void VariableExpander::expand_exported_env_vars_only(std::string& arg) {
    std::string result;
    result.reserve(arg.length() + arg.length() / 2);
    bool in_var = false;
    std::string var_name;
    var_name.reserve(64);

    for (size_t i = 0; i < arg.length(); ++i) {
        if (try_append_arithmetic_expansion(
                arg, i, result, [this](std::string& s) { expand_exported_env_vars_only(s); },
                true)) {
            continue;
        }

        if (arg[i] == '$' && !in_var) {
            in_var = true;
            var_name.clear();
        } else if (in_var) {
            if (!var_name.empty() &&
                ((std::isdigit(static_cast<unsigned char>(var_name.front())) != 0) ||
                 parameter_utils::is_special_parameter_char(var_name.front()) || var_name == "-")) {
                result += get_exported_variable_value(var_name);
                in_var = false;
                var_name.clear();
                --i;
                continue;
            }

            if (arg[i] == '{') {
                size_t brace_start = i + 1;
                size_t brace_end = brace_start;
                int brace_depth = 1;

                while (brace_end < arg.length() && brace_depth > 0) {
                    if (arg[brace_end] == '{') {
                        brace_depth++;
                    } else if (arg[brace_end] == '}') {
                        brace_depth--;
                    }
                    if (brace_depth > 0) {
                        brace_end++;
                    }
                }

                if (brace_depth == 0) {
                    std::string param_expr = arg.substr(brace_start, brace_end - brace_start);
                    std::string value;

                    if (param_expr.find(":-") != std::string::npos ||
                        param_expr.find(":=") != std::string::npos) {
                        value = expand_parameter_with_default(
                            param_expr,
                            [this](const std::string& name) {
                                return get_exported_variable_value(name);
                            },
                            [this](std::string& val) { expand_exported_env_vars_only(val); });
                    } else {
                        value = get_exported_variable_value(param_expr);
                    }

                    result += value;
                    i = brace_end;
                    in_var = false;
                } else {
                    result += arg[i];
                }
            } else if ((std::isalnum(arg[i]) != 0) || arg[i] == '_' || arg[i] == '?' ||
                       arg[i] == '$' || arg[i] == '#' || arg[i] == '*' || arg[i] == '@' ||
                       arg[i] == '!' || arg[i] == '-' || (std::isdigit(arg[i]) != 0)) {
                var_name += arg[i];
            } else {
                std::string value = get_exported_variable_value(var_name);
                result += value;

                if (arg[i] != '$' &&
                    (arg[i] != ':' || i + 1 >= arg.length() || arg[i + 1] != '-') &&
                    arg[i] != '-') {
                    result += arg[i];
                } else if (arg[i] == '$') {
                    i--;
                }
                in_var = false;
                var_name.clear();
            }
        } else {
            result += arg[i];
        }
    }

    if (in_var) {
        std::string value = get_exported_variable_value(var_name);
        result += value;
    }

    arg = result;
}

void VariableExpander::expand_command_substitutions_in_string(std::string& text) {
    if (text.find('$') == std::string::npos && text.find('`') == std::string::npos) {
        return;
    }

    std::string result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '(') {
            size_t pos = find_matching_paren(text, i + 1);
            if (pos != std::string::npos) {
                std::string command = text.substr(i + 2, pos - (i + 2));
                auto output = exec_utils::execute_command_for_output(command);
                if (output.success) {
                    result += trim_command_output(output.output);
                    i = pos + 1;
                    continue;
                }
            }
        } else if (text[i] == '`') {
            size_t pos = i + 1;
            while (pos < text.size()) {
                if (text[pos] == '\\' && pos + 1 < text.size()) {
                    pos += 2;
                    continue;
                }
                if (text[pos] == '`') {
                    break;
                }
                pos++;
            }
            if (pos < text.size() && text[pos] == '`') {
                std::string command = text.substr(i + 1, pos - (i + 1));
                std::string cleaned;
                cleaned.reserve(command.size());
                for (size_t k = 0; k < command.size(); ++k) {
                    if (command[k] == '\\' && k + 1 < command.size() && command[k + 1] == '`') {
                        cleaned.push_back('`');
                        ++k;
                    } else {
                        cleaned.push_back(command[k]);
                    }
                }
                auto output = exec_utils::execute_command_for_output(cleaned);
                if (output.success) {
                    result += trim_command_output(output.output);
                    i = pos + 1;
                    continue;
                }
            }
        }

        result += text[i];
        ++i;
    }

    text.swap(result);
}

bool VariableExpander::try_append_arithmetic_expansion(
    const std::string& arg, size_t& i, std::string& result,
    const std::function<void(std::string&)>& expand_func, bool default_zero_on_empty) {
    auto [handled, arith_result] =
        try_expand_arithmetic_expression(arg, i, expand_func, [this](const std::string& s) {
            if (shell != nullptr) {
                if (auto* interpreter = shell->get_shell_script_interpreter()) {
                    return interpreter->evaluate_arithmetic_expression(s);
                }
                if (auto* parser = shell->get_parser()) {
                    return parser->evaluate_arithmetic(s);
                }
            }
            return 0LL;
        });

    if (!handled) {
        return false;
    }

    if (default_zero_on_empty && arith_result.empty()) {
        result += '0';
    } else {
        result += arith_result;
    }
    return true;
}

void VariableExpander::expand_command_paths_with_home(Command& cmd, const std::string& home) {
    (void)home;
    std::string cwd;
    std::string previous_directory;
    bool directories_resolved = false;

    auto expand_path = [&](std::string& path) {
        if (path.empty() || path.front() != '~') {
            return;
        }

        if (!directories_resolved) {
            cwd = cjsh_filesystem::safe_current_directory();
            previous_directory =
                (shell != nullptr) ? shell->get_previous_directory() : std::string{};
            directories_resolved = true;
        }
        std::filesystem::path expanded =
            cjsh_filesystem::expand_shell_path_token(path, cwd, previous_directory);
        if (!expanded.empty()) {
            path = expanded.string();
        }
    };

    expand_path(cmd.input_file);
    expand_path(cmd.output_file);
    expand_path(cmd.append_file);
    expand_path(cmd.stderr_file);
    expand_path(cmd.both_output_file);

    for (auto& fd_redir : cmd.fd_redirections) {
        expand_path(fd_redir.second);
    }

    for (auto& redirection : cmd.redirection_order) {
        expand_path(redirection.value);
    }
}

void VariableExpander::expand_command_redirection_paths(Command& cmd) {
    auto expand_vars_in_path = [this](std::string& path) {
        if (!path.empty()) {
            expand_env_vars(path);
        }
    };

    expand_vars_in_path(cmd.input_file);
    expand_vars_in_path(cmd.output_file);
    expand_vars_in_path(cmd.append_file);
    expand_vars_in_path(cmd.stderr_file);
    expand_vars_in_path(cmd.both_output_file);

    for (auto& fd_redir : cmd.fd_redirections) {
        std::string& spec = fd_redir.second;
        if (spec.rfind("input:", 0) == 0) {
            std::string path = spec.substr(6);
            expand_vars_in_path(path);
            spec = "input:" + path;
        } else if (spec.rfind("output:", 0) == 0) {
            std::string path = spec.substr(7);
            expand_vars_in_path(path);
            spec = "output:" + path;
        } else {
            expand_vars_in_path(spec);
        }
    }

    for (auto& redirection : cmd.redirection_order) {
        expand_vars_in_path(redirection.value);
    }
}

bool VariableExpander::get_use_exported_vars_only() const {
    return use_exported_vars_only;
}
