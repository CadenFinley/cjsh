/*
  cjshopt_misc_commands.cpp

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

#include "cjshopt_command.h"
#include "isocline.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cjsh_completions.h"
#include "error_out.h"
#include "isocline/isocline.h"
#include "shell_env.h"
#include "string_utils.h"
#include "token_constants.h"

namespace {

bool is_help_flag(const std::string& option) {
    return option == "--help" || option == "-h";
}

std::string resolve_style_registry_name(const std::string& token_type) {
    if (token_type.rfind("ic-", 0) == 0) {
        return token_type;
    }
    return "cjsh-" + token_type;
}

std::string style_preview_sample(const std::string& token_type) {
    if (token_type == "unknown-command") {
        return "notarealcmd";
    }
    if (token_type == "agent-prefix") {
        return "ai: ";
    }
    if (token_type == "agent-request") {
        return "list files changed today";
    }
    if (token_type == "colon") {
        return ":";
    }
    if (token_type == "file-argument") {
        return "README.md";
    }
    if (token_type == "path-exists") {
        return "/usr";
    }
    if (token_type == "path-not-exists") {
        return "/nope";
    }
    if (token_type == "glob-pattern") {
        return "*.cpp";
    }
    if (token_type == "operator") {
        return "&& || | >";
    }
    if (token_type == "keyword") {
        return "if then fi";
    }
    if (token_type == "builtin") {
        return "cd";
    }
    if (token_type == "system") {
        return "ls";
    }
    if (token_type == "variable") {
        return "$HOME";
    }
    if (token_type == "assignment-value") {
        return "FOO=bar";
    }
    if (token_type == "string") {
        return "\"hello\"";
    }
    if (token_type == "heredoc-delimiter") {
        return "EOF";
    }
    if (token_type == "comment") {
        return "# comment";
    }
    if (token_type == "command-substitution") {
        return "$(date)";
    }
    if (token_type == "arithmetic") {
        return "$((1+2))";
    }
    if (token_type == "option") {
        return "--help";
    }
    if (token_type == "number") {
        return "42";
    }
    if (token_type == "function-definition") {
        return "myfunc()";
    }
    if (token_type == "history-expansion") {
        return "!!";
    }
    if (token_type == "ic-prompt") {
        return "prompt";
    }
    if (token_type == "ic-linenumbers") {
        return "1";
    }
    if (token_type == "ic-linenumber-current") {
        return "2";
    }
    if (token_type == "ic-info") {
        return "info";
    }
    if (token_type == "ic-source") {
        return "source";
    }
    if (token_type == "ic-diminish") {
        return "dim";
    }
    if (token_type == "ic-emphasis") {
        return "emphasis";
    }
    if (token_type == "ic-hint") {
        return "hint";
    }
    if (token_type == "ic-error") {
        return "error";
    }
    if (token_type == "ic-bracematch") {
        return "{}";
    }
    if (token_type == "ic-whitespace-char") {
        return "space";
    }
    return token_type;
}

std::vector<std::string> sorted_default_style_token_types() {
    std::vector<std::string> token_types;
    token_types.reserve(token_constants::default_styles().size());
    for (const auto& pair : token_constants::default_styles()) {
        token_types.push_back(pair.first);
    }
    std::sort(token_types.begin(), token_types.end());
    return token_types;
}

void print_style_preview() {
    const auto token_types = sorted_default_style_token_types();

    ic_println("Syntax style preview:");
    for (const auto& token_type : token_types) {
        std::string style_name = resolve_style_registry_name(token_type);
        std::string sample = style_preview_sample(token_type);
        std::string line = token_type;
        line.append(": [").append(style_name).append("]").append(sample).append("[/]");
        ic_println(line.c_str());
    }
    ic_println("Use: cjshopt style_def <token_type> \"<style>\"");
}

void print_style_def_usage() {
    std::cout << "Usage: style_def <token_type> <style>\n";
    std::cout << "       style_def preview\n";
    std::cout << "       style_def --reset\n\n";
    std::cout << "Define, preview, or reset syntax highlighting styles.\n\n";
    std::cout << "Token types:\n";

    const auto token_types = sorted_default_style_token_types();

    for (const auto& token_type : token_types) {
        std::cout << "  " << token_type
                  << " (default: " << token_constants::default_styles().at(token_type) << ")\n";
    }

    std::cout << "\nStyle format: [bold] [italic] [underline] color=#RRGGBB|color=name\n";
    std::cout << "Color names: red, green, blue, yellow, magenta, cyan, white, black\n";
    std::cout << "ANSI colors: ansi-black, ansi-red, ansi-green, ansi-yellow, etc.\n\n";
    std::cout << "Examples:\n";
    std::cout << "  style_def builtin \"bold color=#FFB86C\"\n";
    std::cout << "  style_def system \"color=#50FA7B\"\n";
    std::cout << "  style_def comment \"italic color=green\"\n";
    std::cout << "  style_def string \"color=#F1FA8C\"\n\n";
    std::cout << "To reset all styles to defaults, use: style_def --reset\n";
    std::cout << "To preview current styles, use: style_def preview\n";
}
}  // namespace

int style_def_command(const std::vector<std::string>& args) {
    if (args.size() == 1 || (args.size() == 2 && is_help_flag(args[1]))) {
        if (!cjsh_env::startup_active()) {
            print_style_def_usage();
        }
        return 0;
    }

    if (args.size() == 2 && (args[1] == "preview" || args[1] == "--preview")) {
        if (!cjsh_env::startup_active()) {
            print_style_preview();
        }
        return 0;
    }

    if (args.size() == 2 && args[1] == "--reset") {
        for (const auto& pair : token_constants::default_styles()) {
            ic_style_def(pair.first.c_str(), pair.second.c_str());
        }
        if (!cjsh_env::startup_active()) {
            std::cout << "All syntax highlighting styles reset to defaults.\n";
        }
        return 0;
    }

    if (args.size() != 3) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "style_def",
                     "expected 2 arguments: <token_type> <style>",
                     {"Use 'cjshopt style_def --help' to see available token types"}});
        return 1;
    }

    const std::string& token_type = args[1];
    const std::string& style = args[2];

    if (token_constants::default_styles().find(token_type) ==
        token_constants::default_styles().end()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "style_def",
                     "unknown token type: " + token_type,
                     {"Use 'cjshopt style_def --help' to see available token types"}});
        return 1;
    }

    apply_custom_style(token_type, style);

    return 0;
}

void apply_custom_style(const std::string& token_type, const std::string& style) {
    std::string full_style_name = resolve_style_registry_name(token_type);
    ic_style_def(full_style_name.c_str(), style.c_str());
}

namespace {

struct NumericLimitCommandSpec {
    const char* command_name;
    const std::vector<std::string>* usage_lines;
    long default_value;
    long minimum_value;
    std::function<long()> get_current;
    std::function<bool(long, std::string*)> set_value;
    std::function<void(long)> print_status;
    std::function<void(long)> print_applied;
};

int handle_numeric_limit_command(const std::vector<std::string>& args,
                                 const NumericLimitCommandSpec& spec) {
    const auto& usage_lines = *spec.usage_lines;

    if (args.size() == 1) {
        if (!cjsh_env::startup_active()) {
            for (const auto& line : usage_lines) {
                std::cout << line << '\n';
            }
        }
        print_error(
            {ErrorType::INVALID_ARGUMENT, spec.command_name, "expected 1 argument", usage_lines});
        return 1;
    }

    if (args.size() > 2) {
        print_error({ErrorType::INVALID_ARGUMENT, spec.command_name, "too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    std::string normalized = string_utils::to_lower_copy(option);

    if (normalized == "--help" || normalized == "-h") {
        if (!cjsh_env::startup_active()) {
            for (const auto& line : usage_lines) {
                std::cout << line << '\n';
            }
        }
        return 0;
    }

    if (normalized == "status" || normalized == "--status") {
        if (!cjsh_env::startup_active()) {
            spec.print_status(spec.get_current());
        }
        return 0;
    }

    long requested_limit = 0;
    if (normalized == "default" || normalized == "--default") {
        requested_limit = spec.default_value;
    } else {
        try {
            requested_limit = std::stol(option);
        } catch (const std::invalid_argument&) {
            print_error({ErrorType::INVALID_ARGUMENT, spec.command_name,
                         "invalid number: " + option, usage_lines});
            return 1;
        } catch (const std::out_of_range&) {
            print_error({ErrorType::INVALID_ARGUMENT, spec.command_name,
                         "number out of range: " + option, usage_lines});
            return 1;
        }
    }

    if (requested_limit < spec.minimum_value) {
        print_error({ErrorType::INVALID_ARGUMENT, spec.command_name,
                     "value must be greater than or equal to " + std::to_string(spec.minimum_value),
                     usage_lines});
        return 1;
    }

    std::string error_message;
    if (!spec.set_value(requested_limit, &error_message)) {
        if (error_message.empty()) {
            error_message = "Failed to update value.";
        }
        print_error({ErrorType::RUNTIME_ERROR, spec.command_name, error_message, {}});
        return 1;
    }

    if (!cjsh_env::startup_active()) {
        spec.print_applied(spec.get_current());
    }

    return 0;
}

}  // namespace

int set_history_max_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: set-history-max <number|default|status>",
        "",
        "Configure the maximum number of entries written to the history file.",
        "Use 0 to disable history persistence entirely.",
        "Use 'default' to restore the built-in limit (" +
            std::to_string(get_history_default_history_limit()) + " entries).",
        "Use 'status' to view the current setting.",
        "Minimum value: " + std::to_string(get_history_min_history_limit()) + " (no upper limit)."};

    NumericLimitCommandSpec spec{
        "set-history-max",
        &usage_lines,
        get_history_default_history_limit(),
        get_history_min_history_limit(),
        [] { return get_history_max_entries(); },
        [](long value, std::string* error) { return set_history_max_entries(value, error); },
        [](long current_limit) {
            if (current_limit <= 0) {
                std::cout << "History persistence is currently disabled.\n";
            } else {
                std::cout << "History file retains up to " << current_limit << " entries.\n";
            }
        },
        [](long applied_limit) {
            if (applied_limit <= 0) {
                std::cout << "History persistence disabled.\n";
            } else {
                std::cout << "History file will retain up to " << applied_limit << " entries.\n";
            }
        }};

    return handle_numeric_limit_command(args, spec);
}

int set_completion_max_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: set-completion-max <number|default|status>",
        "",
        "Configure the maximum number of completion entries shown in menus.",
        "Use 'default' to restore the built-in limit (" +
            std::to_string(get_completion_default_max_results()) + " entries).",
        "Use 'status' to view the current setting.",
        "Minimum value: " + std::to_string(get_completion_min_allowed_results()) +
            " (no upper limit)."};

    NumericLimitCommandSpec spec{
        "set-completion-max",
        &usage_lines,
        get_completion_default_max_results(),
        get_completion_min_allowed_results(),
        [] { return get_completion_max_results(); },
        [](long value, std::string* error) { return set_completion_max_results(value, error); },
        [](long current_limit) {
            std::cout << "Completion menu currently shows up to " << current_limit << " entries.\n";
        },
        [](long applied_limit) {
            std::cout << "Completion menu will display up to " << applied_limit << " entries.\n";
        }};

    return handle_numeric_limit_command(args, spec);
}
