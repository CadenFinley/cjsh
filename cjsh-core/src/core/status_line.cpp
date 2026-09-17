/*
  status_line.cpp

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

#include "status_line.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "builtin.h"
#include "builtins_completions_handler.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "error_out.h"
#include "external_sub_completions.h"
#include "interpreter.h"
#include "isocline.h"
#include "pipeline_status_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "suggestion_utils.h"
#include "validation/command_analysis.h"

namespace status_line {
namespace {

struct UnknownCommandInfo {
    std::string command;
    std::vector<std::string> suggestions;
};

struct AutoCdInfo {
    std::string token;
    std::string target;
};

constexpr const char* kStatusCallbackInputVar = "CJSH_STATUS_INPUT";
constexpr const char* kStatusCallbackOutputVar = "CJSH_STATUS_OUTPUT";
constexpr size_t kStatusCallbackMaxLines = 6;
constexpr size_t kStatusCallbackMaxBytes = 2048;

class ScopedShellVariableRestore {
   public:
    explicit ScopedShellVariableRestore(const char* name) : name_(name != nullptr ? name : "") {
        if (!name_.empty() && cjsh_env::shell_variable_is_set(name_)) {
            had_value_ = true;
            previous_value_ = cjsh_env::get_shell_variable_value(name_);
        }
    }

    ~ScopedShellVariableRestore() {
        if (name_.empty()) {
            return;
        }

        if (had_value_) {
            (void)cjsh_env::set_shell_variable_value(name_, previous_value_);
        } else {
            (void)cjsh_env::unset_shell_variable_value(name_);
        }
    }

   private:
    std::string name_;
    bool had_value_ = false;
    std::string previous_value_;
};

class ScopedProcessEnvRestore {
   public:
    explicit ScopedProcessEnvRestore(const char* name) : name_(name != nullptr ? name : "") {
        if (name_.empty()) {
            return;
        }

        const char* current = std::getenv(name_.c_str());
        if (current != nullptr) {
            had_value_ = true;
            previous_value_ = current;
        }
    }

    ~ScopedProcessEnvRestore() {
        if (name_.empty()) {
            return;
        }

        if (had_value_) {
            (void)setenv(name_.c_str(), previous_value_.c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

   private:
    std::string name_;
    bool had_value_ = false;
    std::string previous_value_;
};

struct ScopedBoolFlag {
    explicit ScopedBoolFlag(bool* flag) : flag(flag) {
        if (this->flag != nullptr) {
            *this->flag = true;
        }
    }

    ~ScopedBoolFlag() {
        if (flag != nullptr) {
            *flag = false;
        }
    }

    ScopedBoolFlag(const ScopedBoolFlag&) = delete;
    ScopedBoolFlag& operator=(const ScopedBoolFlag&) = delete;

   private:
    bool* flag;
};

std::string g_user_status_callback_function;
std::string g_transient_status_message;
thread_local bool g_user_status_callback_active = false;

std::string sanitize_for_status(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    std::string sanitized;
    sanitized.reserve(text.size());
    bool previous_space = false;

    for (char ch : text) {
        unsigned char uch = static_cast<unsigned char>(ch);
        char normalized = ch;

        if (normalized == '\n' || normalized == '\r' || normalized == '\t') {
            normalized = ' ';
        }

        if (std::iscntrl(uch) && normalized != ' ') {
            continue;
        }

        if (normalized == ' ') {
            if (sanitized.empty() || previous_space) {
                previous_space = true;
                continue;
            }
            previous_space = true;
        } else {
            previous_space = false;
        }

        sanitized.push_back(normalized);
    }

    size_t start = sanitized.find_first_not_of(' ');
    if (start == std::string::npos) {
        return {};
    }
    if (start > 0) {
        (void)sanitized.erase(0, start);
    }

    size_t end = sanitized.find_last_not_of(' ');
    if (end != std::string::npos && end + 1 < sanitized.size()) {
        (void)sanitized.erase(end + 1);
    }

    return sanitized;
}

std::string sanitize_status_callback_output(const std::string& raw_output) {
    if (raw_output.empty()) {
        return {};
    }

    std::string sanitized;
    sanitized.reserve(std::min(raw_output.size(), kStatusCallbackMaxBytes));

    std::string current_line;
    current_line.reserve(128);
    size_t emitted_lines = 0;

    auto emit_line = [&] {
        if (emitted_lines >= kStatusCallbackMaxLines ||
            sanitized.size() >= kStatusCallbackMaxBytes) {
            current_line.clear();
            return;
        }

        std::string cleaned = sanitize_for_status(current_line);
        current_line.clear();
        if (cleaned.empty()) {
            return;
        }

        if (!sanitized.empty()) {
            if (sanitized.size() >= kStatusCallbackMaxBytes) {
                return;
            }
            sanitized.push_back('\n');
        }

        if (sanitized.size() >= kStatusCallbackMaxBytes) {
            return;
        }

        size_t remaining = kStatusCallbackMaxBytes - sanitized.size();
        if (cleaned.size() > remaining) {
            (void)cleaned.erase(remaining);
        }

        (void)sanitized.append(cleaned);
        emitted_lines++;
    };

    for (char ch : raw_output) {
        if (ch == '\n' || ch == '\r') {
            emit_line();
            if (emitted_lines >= kStatusCallbackMaxLines ||
                sanitized.size() >= kStatusCallbackMaxBytes) {
                break;
            }
            continue;
        }

        current_line.push_back(ch);
    }

    if (!current_line.empty() && emitted_lines < kStatusCallbackMaxLines &&
        sanitized.size() < kStatusCallbackMaxBytes) {
        emit_line();
    }

    return sanitized;
}

std::string build_user_status_callback_message(Shell* shell, const std::string& input) {
    if (shell == nullptr || g_user_status_callback_function.empty()) {
        return {};
    }

    if (g_user_status_callback_active) {
        return {};
    }

    ShellScriptInterpreter* interpreter = shell->get_shell_script_interpreter();
    if (interpreter == nullptr || !interpreter->has_function(g_user_status_callback_function)) {
        return {};
    }

    ScopedShellVariableRestore input_restore(kStatusCallbackInputVar);
    ScopedShellVariableRestore output_restore(kStatusCallbackOutputVar);
    ScopedShellVariableRestore pipe_status_restore("PIPESTATUS");
    ScopedProcessEnvRestore status_restore("?");

    int previous_status_code = 0;
    if (const char* status_env = std::getenv("?"); status_env != nullptr && status_env[0] != '\0') {
        char* end = nullptr;
        long parsed = std::strtol(status_env, &end, 10);
        if (end != nullptr && *end == '\0') {
            previous_status_code = static_cast<int>(parsed);
        }
    }

    (void)cjsh_env::set_shell_variable_value(kStatusCallbackInputVar, input);
    (void)cjsh_env::set_shell_variable_value(kStatusCallbackOutputVar, "");

    ScopedBoolFlag callback_guard(&g_user_status_callback_active);

    try {
        std::vector<std::string> callback_args;
        callback_args.reserve(2);
        (void)callback_args.emplace_back(g_user_status_callback_function);
        (void)callback_args.emplace_back(input);
        (void)interpreter->invoke_function(callback_args);
    } catch (...) {
        pipeline_status_utils::set_last_status_env(previous_status_code);
        return {};
    }

    pipeline_status_utils::set_last_status_env(previous_status_code);

    return sanitize_status_callback_output(
        cjsh_env::get_shell_variable_value(kStatusCallbackOutputVar));
}

bool has_exited_token_context(const std::string& input, size_t absolute_token_end) {
    if (absolute_token_end >= input.size()) {
        return false;
    }

    char next_char = input[absolute_token_end];
    if ((std::isspace(static_cast<unsigned char>(next_char)) != 0)) {
        return true;
    }

    return next_char == '|' || next_char == '&' || next_char == ';';
}

bool token_ready_for_status(const std::string& input, size_t absolute_token_end) {
    if (absolute_token_end >= input.size()) {
        return true;
    }
    return has_exited_token_context(input, absolute_token_end);
}

std::vector<std::string> extract_candidate_commands(const std::vector<std::string>& suggestions) {
    std::vector<std::string> commands;
    commands.reserve(std::min<size_t>(suggestions.size(), 3));

    for (const auto& suggestion : suggestions) {
        size_t first_quote = suggestion.find('\'');
        if (first_quote == std::string::npos) {
            continue;
        }
        size_t second_quote = suggestion.find('\'', first_quote + 1);
        if (second_quote == std::string::npos || second_quote <= first_quote + 1) {
            continue;
        }

        (void)commands.emplace_back(
            suggestion.substr(first_quote + 1, second_quote - first_quote - 1));
        if (commands.size() == 3) {
            break;
        }
    }

    return commands;
}

std::vector<std::string> gather_candidate_commands_for_query(const std::string& query) {
    if (query.empty()) {
        return {};
    }

    auto suggestions = suggestion_utils::generate_command_suggestions_if_enabled(query);
    return extract_candidate_commands(suggestions);
}

UnknownCommandInfo build_unknown_command_info(const std::string& token) {
    UnknownCommandInfo info;
    info.command = token;
    info.suggestions = gather_candidate_commands_for_query(token);
    return info;
}

std::optional<UnknownCommandInfo> analyze_command_range(
    Shell* shell, const std::string& original_input, const std::string& analysis,
    const std::unordered_set<std::string>& available_commands, size_t cmd_start, size_t cmd_end) {
    std::string cmd_str = analysis.substr(cmd_start, cmd_end - cmd_start);

    size_t token_cursor = 0;
    size_t first_token_start = 0;
    size_t first_token_end = 0;
    if (!command_analysis::extract_next_token(cmd_str, token_cursor, first_token_start,
                                              first_token_end)) {
        return std::nullopt;
    }

    std::string token = cmd_str.substr(first_token_start, first_token_end - first_token_start);
    size_t absolute_token_end = cmd_start + first_token_end;

    bool token_unknown =
        !command_analysis::is_known_command_token(token, cmd_start, shell, available_commands) &&
        !token.empty();
    if (token_unknown && has_exited_token_context(original_input, absolute_token_end)) {
        return build_unknown_command_info(token);
    }

    if (token == "sudo") {
        size_t arg_cursor = token_cursor;
        size_t arg_start = 0;
        size_t arg_end = 0;
        if (command_analysis::extract_next_token(cmd_str, arg_cursor, arg_start, arg_end)) {
            std::string arg = cmd_str.substr(arg_start, arg_end - arg_start);
            size_t absolute_arg_end = cmd_start + arg_end;
            bool arg_unknown = !command_analysis::is_known_command_token(
                                   arg, cmd_start + arg_start, shell, available_commands) &&
                               !arg.empty();
            if (arg_unknown && has_exited_token_context(original_input, absolute_arg_end)) {
                return build_unknown_command_info(arg);
            }
        }
    }

    return std::nullopt;
}

std::optional<UnknownCommandInfo> detect_unknown_command(Shell* shell,
                                                         const std::string& original_input) {
    if (shell == nullptr || original_input.empty()) {
        return std::nullopt;
    }

    std::string analysis = command_analysis::sanitize_input_for_analysis(original_input);
    if (analysis.empty()) {
        return std::nullopt;
    }

    std::unordered_set<std::string> available_commands = shell->get_available_commands();

    std::optional<UnknownCommandInfo> result;
    (void)command_analysis::visit_command_ranges(
        analysis, [&](size_t command_start, size_t command_end) {
            result = analyze_command_range(shell, original_input, analysis, available_commands,
                                           command_start, command_end);
            return !result.has_value();
        });
    return result;
}

bool is_auto_cd_token(const std::string& token, Shell* shell) {
    return command_lookup::should_auto_cd_token(token, shell);
}

std::string resolve_auto_cd_target(const std::string& token, Shell* shell) {
    if (shell == nullptr || shell->get_built_ins() == nullptr || token.empty()) {
        return {};
    }

    const std::string cwd = shell->get_built_ins()->get_current_directory();
    const std::string previous_directory = shell->get_previous_directory();
    return cjsh_filesystem::resolve_existing_shell_directory_token(token, cwd, previous_directory);
}

std::optional<AutoCdInfo> detect_auto_cd_command(Shell* shell, const std::string& original_input) {
    if (shell == nullptr || original_input.empty()) {
        return std::nullopt;
    }

    std::string analysis = command_analysis::sanitize_input_for_analysis(original_input);
    if (analysis.empty()) {
        return std::nullopt;
    }

    std::optional<AutoCdInfo> result;
    (void)command_analysis::visit_command_ranges(analysis, [&](size_t command_start,
                                                               size_t command_end) {
        std::string cmd_str = analysis.substr(command_start, command_end - command_start);
        size_t token_cursor = 0;
        size_t token_start = 0;
        size_t token_end = 0;
        if (command_analysis::extract_next_token(cmd_str, token_cursor, token_start, token_end)) {
            std::string token = cmd_str.substr(token_start, token_end - token_start);
            size_t absolute_token_end = command_start + token_end;
            if (token_ready_for_status(original_input, absolute_token_end) &&
                is_auto_cd_token(token, shell)) {
                result = AutoCdInfo{token, resolve_auto_cd_target(token, shell)};
                return false;
            }
        }
        return true;
    });
    return result;
}

std::string format_suggestion_list(const std::vector<std::string>& suggestions) {
    if (suggestions.empty()) {
        return {};
    }

    std::vector<std::string> sanitized;
    sanitized.reserve(suggestions.size());
    for (const auto& entry : suggestions) {
        std::string cleaned = sanitize_for_status(entry);
        if (!cleaned.empty()) {
            sanitized.push_back(cleaned);
        }
    }

    if (sanitized.empty()) {
        return {};
    }

    if (sanitized.size() == 1) {
        return sanitized.front();
    }

    if (sanitized.size() == 2) {
        return sanitized[0] + " or " + sanitized[1];
    }

    std::string result = sanitized[0];
    (void)result.append(", ");
    (void)result.append(sanitized[1]);
    (void)result.append(", or ");
    (void)result.append(sanitized[2]);
    return result;
}

std::string format_unknown_command_message(const UnknownCommandInfo& info) {
    std::string sanitized_command = sanitize_for_status(info.command);
    if (sanitized_command.empty()) {
        sanitized_command = info.command;
    }

    std::string message = "Unknown command: ";
    (void)message.append(sanitized_command);

    if (config::error_suggestions_enabled && !info.suggestions.empty()) {
        std::string suggestion_text = format_suggestion_list(info.suggestions);
        if (!suggestion_text.empty()) {
            (void)message.append(" | Did you mean: ");
            (void)message.append(suggestion_text);
            message.push_back('?');
        }
    }

    return message;
}

std::string format_auto_cd_message(const AutoCdInfo& info) {
    std::string token = sanitize_for_status(info.token);
    if (token.empty()) {
        token = info.token;
    }

    std::string message = "Auto cd dir: ";
    (void)message.append(token);
    if (!info.target.empty()) {
        std::string target = sanitize_for_status(info.target);
        if (!target.empty()) {
            (void)message.append(" -> ");
            (void)message.append(target);
        }
    }
    return message;
}

const char* severity_to_label(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::CRITICAL:
            return "critical";
        case ErrorSeverity::ERROR:
            return "error";
        case ErrorSeverity::WARNING:
            return "warning";
        case ErrorSeverity::INFO:
        default:
            return "info";
    }
}

constexpr const char* kAnsiReset = "\x1b[0m";

const char* severity_to_underline_style(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::CRITICAL:
            return "\x1b[4m\x1b[58;5;196m";
        case ErrorSeverity::ERROR:
            return "\x1b[4m\x1b[58;5;160m";
        case ErrorSeverity::WARNING:
            return "\x1b[4m\x1b[58;5;214m";
        case ErrorSeverity::INFO:
        default:
            return "\x1b[4m\x1b[58;5;51m";
    }
}

std::string format_error_location(const ShellScriptInterpreter::SyntaxError& error) {
    const auto& pos = error.position;
    if (pos.line_number == 0) {
        return {};
    }

    std::string location = "line ";
    location += std::to_string(pos.line_number);
    if (pos.column_start > 0) {
        location += ", col ";
        location += std::to_string(pos.column_start + 1);
    }
    return location;
}

std::string build_validation_status_message(
    const std::vector<ShellScriptInterpreter::SyntaxError>& errors) {
    if (errors.empty()) {
        return {};
    }

    struct SeverityBuckets {
        size_t info = 0;
        size_t warning = 0;
        size_t error = 0;
        size_t critical = 0;
    } buckets;

    std::vector<const ShellScriptInterpreter::SyntaxError*> sorted_errors;
    sorted_errors.reserve(errors.size());

    for (const auto& err : errors) {
        switch (err.severity) {
            case ErrorSeverity::CRITICAL:
                ++buckets.critical;
                break;
            case ErrorSeverity::ERROR:
                ++buckets.error;
                break;
            case ErrorSeverity::WARNING:
                ++buckets.warning;
                break;
            case ErrorSeverity::INFO:
            default:
                ++buckets.info;
                break;
        }

        sorted_errors.push_back(&err);
    }

    auto severity_rank = [](ErrorSeverity severity) {
        switch (severity) {
            case ErrorSeverity::CRITICAL:
                return 3;
            case ErrorSeverity::ERROR:
                return 2;
            case ErrorSeverity::WARNING:
                return 1;
            case ErrorSeverity::INFO:
            default:
                return 0;
        }
    };

    std::sort(sorted_errors.begin(), sorted_errors.end(), [&](const auto* lhs, const auto* rhs) {
        int lhs_rank = severity_rank(lhs->severity);
        int rhs_rank = severity_rank(rhs->severity);
        if (lhs_rank != rhs_rank) {
            return lhs_rank > rhs_rank;
        }
        if (lhs->position.line_number != rhs->position.line_number) {
            return lhs->position.line_number < rhs->position.line_number;
        }
        if (lhs->position.column_start != rhs->position.column_start) {
            return lhs->position.column_start < rhs->position.column_start;
        }
        return lhs->message < rhs->message;
    });

    if (sorted_errors.empty()) {
        return {};
    }

    std::vector<std::string> counter_parts;
    counter_parts.reserve(4);

    auto append_part = [&counter_parts](size_t count, const char* label) {
        if (count == 0) {
            return;
        }
        std::string part = std::to_string(count);
        part.push_back(' ');
        (void)part.append(label);
        if (count > 1) {
            part.push_back('s');
        }
        counter_parts.push_back(std::move(part));
    };

    append_part(buckets.critical, "critical");
    append_part(buckets.error, "error");
    append_part(buckets.warning, "warning");
    append_part(buckets.info, "info");

    std::string message;
    message.reserve(256);

    for (size_t i = 0; i < sorted_errors.size(); ++i) {
        const auto* error = sorted_errors[i];

        if (i != 0) {
            message.push_back('\n');
        }

        std::string line;
        line.reserve(128);
        line.push_back('[');
        (void)line.append(severity_to_label(error->severity));
        (void)line.append("]");

        std::string location = format_error_location(*error);
        std::string sanitized_text = sanitize_for_status(error->message);
        std::string sanitized_suggestion = sanitize_for_status(error->suggestion);
        std::string detail_text;

        if (!location.empty()) {
            (void)detail_text.append(location);
        }
        if (!sanitized_text.empty()) {
            if (!detail_text.empty()) {
                (void)detail_text.append(" - ");
            }
            (void)detail_text.append(sanitized_text);
        }
        if (config::error_suggestions_enabled && !sanitized_suggestion.empty()) {
            if (!detail_text.empty()) {
                (void)detail_text.append(" | ");
            }
            (void)detail_text.append(sanitized_suggestion);
        }

        if (!detail_text.empty()) {
            line.push_back(' ');
            (void)line.append(detail_text);
        }

        const char* style_prefix = severity_to_underline_style(error->severity);
        if (style_prefix != nullptr && style_prefix[0] != '\0') {
            (void)message.append(style_prefix);
            (void)message.append(line);
            (void)message.append(kAnsiReset);
        } else {
            (void)message.append(line);
        }
    }

    return message;
}

std::string build_cjsh_status_reporting_message(Shell* shell, const std::string& current_input) {
    // Limit cached lookups to our analysis; user status callbacks may execute explicit queries.
    const cjsh_filesystem::ScopedInteractivePathLookup path_lookup;
    if (!config::status_reporting_enabled) {
        return {};
    }

    if (current_input.empty()) {
        return {};
    }

    bool has_visible_content = std::any_of(current_input.begin(), current_input.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) == 0;
    });

    if (!has_visible_content || shell == nullptr) {
        return {};
    }

    ShellScriptInterpreter* interpreter = shell->get_shell_script_interpreter();
    if (interpreter == nullptr) {
        return {};
    }

    std::vector<std::string> lines = interpreter->parse_into_lines(current_input);
    if (lines.empty()) {
        (void)lines.emplace_back(current_input);
    }

    std::vector<ShellScriptInterpreter::SyntaxError> errors;
    try {
        errors = interpreter->validate_comprehensive_syntax(lines);
    } catch (const std::exception& ex) {
        std::string failure_message = "Validation failed: ";
        (void)failure_message.append(sanitize_for_status(ex.what()));
        return failure_message;
    } catch (...) {
        return "Validation failed: unknown error.";
    }

    std::optional<AutoCdInfo> auto_cd_info = detect_auto_cd_command(shell, current_input);
    std::optional<UnknownCommandInfo> unknown_info = detect_unknown_command(shell, current_input);
    std::string validation_message = build_validation_status_message(errors);

    std::string combined_message;
    if (auto_cd_info.has_value()) {
        combined_message = format_auto_cd_message(*auto_cd_info);
    } else if (unknown_info.has_value()) {
        combined_message = format_unknown_command_message(*unknown_info);
    }

    if (!validation_message.empty()) {
        if (!combined_message.empty()) {
            combined_message.push_back('\n');
        }
        (void)combined_message.append(validation_message);
    }

    return combined_message;
}

std::string build_command_hint_message(Shell* shell, const std::string& input, size_t cursor_pos) {
    if (shell == nullptr || !config::status_reporting_enabled || cursor_pos > input.size()) {
        return {};
    }

    const cjsh_filesystem::ScopedInteractivePathLookup path_lookup;
    const std::string analysis = command_analysis::sanitize_input_for_analysis(input);
    std::string source;
    std::string description;
    (void)command_analysis::visit_command_ranges(analysis, [&](size_t command_start,
                                                               size_t command_end) {
        if (cursor_pos < command_start || cursor_pos > command_end) {
            return true;
        }

        const std::string command = analysis.substr(command_start, command_end - command_start);
        size_t token_cursor = 0;
        size_t token_start = 0;
        size_t token_end = 0;
        if (!command_analysis::extract_next_token(command, token_cursor, token_start, token_end) ||
            cursor_pos < command_start + token_start || cursor_pos > command_start + token_end) {
            return true;
        }

        const std::string token = command.substr(token_start, token_end - token_start);
        // Only inspect literal command names. Never expand or execute prompt input.
        if (!command_lookup::token_allows_split_command_merge(token) ||
            token.find_first_of("*?~!") != std::string::npos) {
            return false;
        }
        if (shell->get_interactive_mode()) {
            const auto& abbreviations = shell->get_abbreviations();
            const auto abbreviation = abbreviations.find(token);
            if (abbreviation != abbreviations.end()) {
                source = "abbreviation";
                description = abbreviation->second;
                return false;
            }
        }

        const auto resolution = command_lookup::resolve_command(token, shell, false);
        if (resolution.is_keyword) {
            source = "keyword";
            description = builtin_completions::get_builtin_summary(token);
        } else if (resolution.has_alias) {
            source = "alias";
            description = resolution.alias_value;
        } else if (resolution.has_function) {
            source = "function";
            description = token;
        } else if (resolution.is_builtin) {
            source = token;
            description = builtin_completions::get_builtin_summary(token);
        } else {
            source = cjsh_filesystem::find_executable_in_path(token);
            if (!source.empty()) {
                // Reuse completion documentation without fetching man pages while typing.
                description = get_command_summary(token, false);
                if (description == source) {
                    description.clear();
                }
            }
        }
        return false;
    });
    if (source.empty()) {
        return {};
    }

    // Status content is parsed as BBCode. Keep paths and descriptions literal.
    auto escape = [](const std::string& text) {
        std::string escaped;
        for (char ch : sanitize_for_status(text)) {
            if (ch == '[' || ch == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(ch);
        }
        return escaped;
    };
    std::string message = "(" + escape(source) + ")";
    if (config::colors_enabled && config::syntax_highlighting_enabled) {
        // Match the source tag on an unselected completion menu item.
        message = "[ic-diminish]" + message + "[/]";
    }
    const std::string escaped_description = escape(description);
    if (!escaped_description.empty()) {
        message += " - " + escaped_description;
    }
    return message;
}

std::string previous_passed_buffer;
bool previous_passed_buffer_valid = false;

}  // namespace

const char* create_below_syntax_message(const char* input_buffer, void*) {
    size_t cursor_pos = input_buffer != nullptr ? std::char_traits<char>::length(input_buffer) : 0;
    (void)ic_get_cursor_pos(&cursor_pos);
    return create_below_syntax_message_at_cursor(input_buffer, cursor_pos);
}

const char* create_below_syntax_message_at_cursor(const char* input_buffer, size_t cursor_pos) {
    static thread_local std::string status_message;
    static thread_local std::string buffer_message;

    if (!g_transient_status_message.empty()) {
        status_message = g_transient_status_message;
        return status_message.c_str();
    }

    if (!config::status_line_enabled) {
        status_message.clear();
        previous_passed_buffer.clear();
        previous_passed_buffer_valid = false;
        return nullptr;
    }

    const std::string current_input = (input_buffer != nullptr) ? input_buffer : "";

    Shell* shell = g_shell.get();
    // Cursor-only refreshes should not repeat syntax validation or invoke user functions.
    if (!previous_passed_buffer_valid || previous_passed_buffer != current_input) {
        previous_passed_buffer = current_input;
        previous_passed_buffer_valid = true;
        buffer_message = build_user_status_callback_message(shell, current_input);
        const std::string reporting_message =
            build_cjsh_status_reporting_message(shell, current_input);
        if (!reporting_message.empty()) {
            if (!buffer_message.empty()) {
                buffer_message.push_back('\n');
            }
            (void)buffer_message.append(reporting_message);
        }
    }

    status_message = buffer_message;
    const std::string command_hint = build_command_hint_message(shell, current_input, cursor_pos);
    if (!command_hint.empty()) {
        if (!status_message.empty()) {
            status_message.push_back('\n');
        }
        (void)status_message.append(command_hint);
    }

    if (status_message.empty()) {
        return nullptr;
    }

    return status_message.c_str();
}

void set_transient_status_message(const std::string& message) {
    g_transient_status_message = message;
    previous_passed_buffer.clear();
    previous_passed_buffer_valid = false;
}

void clear_transient_status_message() {
    g_transient_status_message.clear();
    previous_passed_buffer.clear();
    previous_passed_buffer_valid = false;
}

void set_user_status_callback_function(const std::string& function_name) {
    g_user_status_callback_function = function_name;
    previous_passed_buffer.clear();
    previous_passed_buffer_valid = false;
}

void clear_user_status_callback_function() {
    g_user_status_callback_function.clear();
    previous_passed_buffer.clear();
    previous_passed_buffer_valid = false;
}

std::string get_user_status_callback_function() {
    return g_user_status_callback_function;
}

}  // namespace status_line
