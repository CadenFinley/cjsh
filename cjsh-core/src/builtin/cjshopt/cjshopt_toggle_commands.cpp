/*
  cjshopt_toggle_commands.cpp

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

#include "builtin_help.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "cjsh_completions.h"
#include "error_out.h"
#include "interpreter.h"
#include "isocline/isocline.h"
#include "numeric_utils.h"
#include "parser_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "status_line.h"
#include "string_utils.h"

namespace {
enum class ToggleRequest : std::uint8_t {
    Enable,
    Disable,
    Status
};

enum class StatusQuery : std::uint8_t {
    Status,
    Value
};

struct ToggleCommandConfig {
    std::string command_name;
    std::vector<std::string> usage_lines;
    std::function<bool()> get_current;
    std::function<void(bool)> set_state;
    std::string status_label;
    bool status_label_is_plural = false;
    std::optional<std::string> persist_template;
    std::vector<std::string> true_synonyms;
    std::vector<std::string> false_synonyms;
};

std::string normalize_option(const std::string& option) {
    return string_utils::to_lower_copy(option);
}

bool matches_token(const std::string& value, std::initializer_list<const char*> tokens) {
    return std::any_of(tokens.begin(), tokens.end(),
                       [&](const char* token) { return value == token; });
}

std::optional<ToggleRequest> parse_toggle_request(const ToggleCommandConfig& config,
                                                  const std::string& normalized) {
    if (matches_token(normalized, {"status", "--status"})) {
        return ToggleRequest::Status;
    }
    if (matches_token(normalized, {"on", "enable", "enabled", "true", "1", "--enable"})) {
        return ToggleRequest::Enable;
    }
    if (matches_token(normalized, {"off", "disable", "disabled", "false", "0", "--disable"})) {
        return ToggleRequest::Disable;
    }

    if (std::any_of(config.true_synonyms.begin(), config.true_synonyms.end(),
                    [&](const std::string& token) { return normalized == token; })) {
        return ToggleRequest::Enable;
    }
    if (std::any_of(config.false_synonyms.begin(), config.false_synonyms.end(),
                    [&](const std::string& token) { return normalized == token; })) {
        return ToggleRequest::Disable;
    }

    return std::nullopt;
}

StatusQuery parse_status_query(const std::string& normalized) {
    if (matches_token(normalized, {"status", "--status"})) {
        return StatusQuery::Status;
    }
    return StatusQuery::Value;
}

std::string format_persist_message(const ToggleCommandConfig& config, bool enable) {
    if (!config.persist_template) {
        return {};
    }

    std::string result = *config.persist_template;
    const std::string state_word = enable ? "on" : "off";

    auto replace_all = [](std::string* target, const std::string& from, const std::string& to) {
        size_t position = 0;
        while ((position = target->find(from, position)) != std::string::npos) {
            (void)target->replace(position, from.size(), to);
            position += to.size();
        }
    };

    replace_all(&result, "{command}", config.command_name);
    replace_all(&result, "{state}", state_word);

    return result;
}

std::string describe_status_hint_mode(ic_status_hint_mode_t mode) {
    switch (mode) {
        case IC_STATUS_HINT_OFF:
            return "hidden (never shown)";
        case IC_STATUS_HINT_NORMAL:
            return "normal (default: only when input and status are empty)";
        case IC_STATUS_HINT_TRANSIENT:
            return "transient (show when the status line is empty)";
        case IC_STATUS_HINT_PERSISTENT:
            return "persistent (always prepended above status lines)";
        default:
            return "unknown";
    }
}

const char* canonical_status_hint_token(ic_status_hint_mode_t mode) {
    switch (mode) {
        case IC_STATUS_HINT_OFF:
            return "off";
        case IC_STATUS_HINT_NORMAL:
            return "normal";
        case IC_STATUS_HINT_TRANSIENT:
            return "transient";
        case IC_STATUS_HINT_PERSISTENT:
            return "persistent";
        default:
            return "normal";
    }
}

std::string describe_menu_highlight_mode(ic_menu_highlight_mode_t mode) {
    switch (mode) {
        case IC_MENU_HIGHLIGHT_NONE:
            return "none (default: menu items are not syntax-highlighted)";
        case IC_MENU_HIGHLIGHT_SINGLE:
            return "single (highlight only the selected item)";
        case IC_MENU_HIGHLIGHT_ALL:
            return "all (highlight every rendered item)";
        case IC_MENU_HIGHLIGHT_REVERSE:
            return "reverse (highlight every rendered item except the selected item)";
        default:
            return "unknown";
    }
}

const char* canonical_menu_highlight_token(ic_menu_highlight_mode_t mode) {
    switch (mode) {
        case IC_MENU_HIGHLIGHT_NONE:
            return "none";
        case IC_MENU_HIGHLIGHT_SINGLE:
            return "single";
        case IC_MENU_HIGHLIGHT_ALL:
            return "all";
        case IC_MENU_HIGHLIGHT_REVERSE:
            return "reverse";
        default:
            return "none";
    }
}

std::optional<ic_menu_highlight_mode_t> parse_menu_highlight_mode(const std::string& normalized) {
    if (matches_token(normalized, {"none", "off", "disable", "disabled", "false", "0"})) {
        return IC_MENU_HIGHLIGHT_NONE;
    }
    if (matches_token(normalized, {"single", "selected", "selection", "cursor"})) {
        return IC_MENU_HIGHLIGHT_SINGLE;
    }
    if (matches_token(normalized, {"all", "on", "enable", "enabled", "true", "1"})) {
        return IC_MENU_HIGHLIGHT_ALL;
    }
    if (matches_token(normalized, {"reverse", "inverse", "inverted"})) {
        return IC_MENU_HIGHLIGHT_REVERSE;
    }
    return std::nullopt;
}

bool g_status_hint_preference_initialized = false;
ic_status_hint_mode_t g_status_hint_preference = IC_STATUS_HINT_NORMAL;

void ensure_status_hint_preference_initialized() {
    if (!g_status_hint_preference_initialized) {
        g_status_hint_preference = ic_get_status_hint_mode();
        g_status_hint_preference_initialized = true;
    }
}

void apply_effective_status_hint_mode() {
    ensure_status_hint_preference_initialized();
    if (config::status_line_enabled) {
        (void)ic_set_status_hint_mode(g_status_hint_preference);
    } else {
        (void)ic_set_status_hint_mode(IC_STATUS_HINT_OFF);
    }
}

int handle_toggle_command(const ToggleCommandConfig& config, const std::vector<std::string>& args) {
    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, config.command_name, "Missing option argument",
                     config.usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, config.usage_lines)) {
        if (!cjsh_env::startup_active()) {
            std::cout << "Current: " << (config.get_current() ? "enabled" : "disabled") << '\n';
        }
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, config.command_name,
                     "Too many arguments provided", config.usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    auto request = parse_toggle_request(config, normalized);
    if (!request.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, config.command_name,
                     "Unknown option '" + option + "'", config.usage_lines});
        return 1;
    }

    if (*request == ToggleRequest::Status) {
        if (!cjsh_env::startup_active()) {
            const char* verb = config.status_label_is_plural ? "are" : "is";
            std::cout << config.status_label << ' ' << verb << " currently "
                      << (config.get_current() ? "enabled" : "disabled") << ".\n";
        }
        return 0;
    }

    bool enable = (*request == ToggleRequest::Enable);

    const bool previously_enabled = config.get_current();
    if (previously_enabled == enable) {
        return 0;
    }

    config.set_state(enable);

    if (!cjsh_env::startup_active()) {
        std::cout << config.status_label << ' ' << (enable ? "enabled" : "disabled") << ".\n";
        const std::string extra = format_persist_message(config, enable);
        if (!extra.empty()) {
            std::cout << extra;
        }
    }

    return 0;
}
}  // namespace

int current_line_number_highlight_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: current-line-number-highlight <on|off|status>", "Examples:",
        "  current-line-number-highlight on      Enable highlighting of the current line number",
        "  current-line-number-highlight off     Disable highlighting of the current line number",
        "  current-line-number-highlight status  Show the current setting"};

    static const ToggleCommandConfig config{
        "current-line-number-highlight",
        usage_lines,
        [] { return ic_current_line_number_highlight_is_enabled(); },
        [](bool enable) { (void)ic_enable_current_line_number_highlight(enable); },
        "Current line number highlighting",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int completion_case_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-case <on|off|status>",
        "Examples:", "  completion-case on       Enable case sensitive completions",
        "  completion-case off      Use case insensitive completions",
        "  completion-case status   Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-case",
        usage_lines,
        [] { return is_completion_case_sensitive(); },
        [](bool enable) { set_completion_case_sensitive(enable); },
        "Completion case sensitivity",
        false,
        std::nullopt,
        {"case-sensitive", "--case-sensitive"},
        {"case-insensitive", "--case-insensitive"}};

    return handle_toggle_command(config, args);
}

int history_search_case_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: history-search-case <on|off|status>", "Examples:",
        "  history-search-case on       Require exact case matches in fuzzy history search",
        "  history-search-case off      Match history entries case insensitively",
        "  history-search-case status   Show the current setting"};

    static const ToggleCommandConfig config{
        "history-search-case",
        usage_lines,
        [] { return ic_history_fuzzy_search_is_case_sensitive(); },
        [](bool enable) { (void)ic_enable_history_fuzzy_case_sensitive(enable); },
        "History search case sensitivity",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {"case-sensitive", "--case-sensitive"},
        {"case-insensitive", "--case-insensitive"}};

    return handle_toggle_command(config, args);
}

int history_directory_command(const std::vector<std::string>& args) {
    static const ToggleCommandConfig config{
        "history-directory",
        {"Usage: history-directory <on|off|status>",
         "Scope interactive history recall to the current directory.",
         "Default: off. Add cjshopt history-directory on to ~/.cjshrc to persist."},
        [] { return ic_history_directory_is_enabled(); },
        [](bool enable) { (void)ic_enable_history_directory(enable); },
        "Directory-aware history",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};
    return handle_toggle_command(config, args);
}

int history_directory_subdirs_command(const std::vector<std::string>& args) {
    static const ToggleCommandConfig config{
        "history-directory-subdirs",
        {"Usage: history-directory-subdirs <on|off|status>",
         "Include commands from nested directories when directory-aware history is enabled.",
         "Default: off. Add cjshopt history-directory-subdirs on to ~/.cjshrc to persist."},
        [] { return ic_history_directory_subdirs_is_enabled(); },
        [](bool enable) { (void)ic_enable_history_directory_subdirs(enable); },
        "History nested directories",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};
    return handle_toggle_command(config, args);
}

int completion_spell_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-spell <on|off|status>",
        "Examples:", "  completion-spell on      Enable spell correction in completions",
        "  completion-spell off     Disable spell correction in completions",
        "  completion-spell status  Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-spell",
        usage_lines,
        [] { return is_completion_spell_correction_enabled(); },
        [](bool enable) { set_completion_spell_correction_enabled(enable); },
        "Completion spell correction",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {"spell", "--spell"},
        {}};

    return handle_toggle_command(config, args);
}

int completion_spell_enter_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-spell-enter <on|off|status>", "Examples:",
        "  completion-spell-enter on      Auto-apply a single spell correction when pressing Enter",
        "  completion-spell-enter off     Submit input as typed when pressing Enter",
        "  completion-spell-enter status  Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-spell-enter",
        usage_lines,
        [] { return is_completion_spell_correction_on_enter_enabled(); },
        [](bool enable) { set_completion_spell_correction_on_enter_enabled(enable); },
        "Enter spell correction",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int completion_learning_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-learning <on|off|status>", "Examples:",
        "  completion-learning on      Allow cjsh to learn completions as you use commands",
        "  completion-learning off     Only use cached completions (run generate-completions)",
        "  completion-learning status  Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-learning",
        usage_lines,
        [] { return config::completion_learning_enabled; },
        [](bool enable) { config::completion_learning_enabled = enable; },
        "Completion learning",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int exit_confirmation_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: exit-confirmation <smart|always|never|status>",
        "Examples:",
        "  exit-confirmation smart   Confirm only when running or stopped jobs exist (default)",
        "  exit-confirmation always  Confirm every exit request",
        "  exit-confirmation never   Never confirm exit requests",
        "  exit-confirmation status  Show the current mode"};

    const auto canonical_mode_token = [](config::ExitConfirmationMode mode) -> const char* {
        switch (mode) {
            case config::ExitConfirmationMode::Smart:
                return "smart";
            case config::ExitConfirmationMode::Always:
                return "always";
            case config::ExitConfirmationMode::Never:
                return "never";
        }
        return "smart";
    };

    const auto describe_mode = [](config::ExitConfirmationMode mode) -> const char* {
        switch (mode) {
            case config::ExitConfirmationMode::Smart:
                return "confirm only when running or stopped jobs exist";
            case config::ExitConfirmationMode::Always:
                return "confirm every exit request";
            case config::ExitConfirmationMode::Never:
                return "never confirm exit requests";
        }
        return "confirm only when running or stopped jobs exist";
    };

    const auto parse_mode =
        [](const std::string& normalized) -> std::optional<config::ExitConfirmationMode> {
        if (matches_token(normalized, {"smart", "auto", "automatic", "default"})) {
            return config::ExitConfirmationMode::Smart;
        }
        if (matches_token(normalized, {"always", "all"})) {
            return config::ExitConfirmationMode::Always;
        }
        if (matches_token(normalized, {"never", "none", "off"})) {
            return config::ExitConfirmationMode::Never;
        }
        return std::nullopt;
    };

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "exit-confirmation", "Missing option argument",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        if (!cjsh_env::startup_active()) {
            const auto mode = config::exit_confirmation_mode;
            std::cout << "Current: " << canonical_mode_token(mode) << " (" << describe_mode(mode)
                      << ").\n";
        }
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "exit-confirmation",
                     "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);
    if (matches_token(normalized, {"status", "--status"})) {
        if (!cjsh_env::startup_active()) {
            const auto mode = config::exit_confirmation_mode;
            std::cout << "Exit confirmation mode is currently " << canonical_mode_token(mode)
                      << " (" << describe_mode(mode) << ").\n";
        }
        return 0;
    }

    const auto requested_mode = parse_mode(normalized);
    if (!requested_mode.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, "exit-confirmation",
                     "Unknown option '" + option + "'", usage_lines});
        return 1;
    }

    if (config::exit_confirmation_mode == *requested_mode) {
        return 0;
    }

    config::exit_confirmation_mode = *requested_mode;
    if (!cjsh_env::startup_active()) {
        std::cout << "Exit confirmation mode set to " << canonical_mode_token(*requested_mode)
                  << " (" << describe_mode(*requested_mode) << ").\n";
        std::cout << "Add `cjshopt exit-confirmation " << canonical_mode_token(*requested_mode)
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int smart_cd_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: smart-cd <on|off|status>",
        "Examples:", "  smart-cd on      Enable smart cd auto-jumps",
        "  smart-cd off     Disable smart cd auto-jumps",
        "  smart-cd status  Show the current setting"};

    static const ToggleCommandConfig config{
        "smart-cd",
        usage_lines,
        [] { return config::smart_cd_enabled; },
        [](bool enable) { config::smart_cd_enabled = enable; },
        "Smart cd",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int extglob_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: extglob <on|off|status>",
        "Examples:", "  extglob on      Enable ?(), *(), +(), @(), and !() patterns",
        "  extglob off     Treat extended glob operators literally",
        "  extglob status  Show the current setting"};

    static const ToggleCommandConfig config{
        "extglob",
        usage_lines,
        [] { return config::extglob_enabled; },
        [](bool enable) { config::extglob_enabled = enable && !config::posix_mode; },
        "Extended glob patterns",
        true,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    if (config::posix_mode && args.size() > 1 && args[1] != "off" && args[1] != "status" &&
        args[1] != "--status") {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "extglob",
                     "extended glob patterns are not available in POSIX mode",
                     {}});
        return 1;
    }
    return handle_toggle_command(config, args);
}

int script_extension_interpreter_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: script-extension-interpreter <on|off|status>",
        "Examples:", "  script-extension-interpreter on      Enable extension-based script runners",
        "  script-extension-interpreter off     Disable extension-based script runners",
        "  script-extension-interpreter status  Show the current setting"};

    static const ToggleCommandConfig config{
        "script-extension-interpreter",
        usage_lines,
        [] { return config::script_extension_interpreter_enabled; },
        [](bool enable) { config::script_extension_interpreter_enabled = enable; },
        "Script extension interpreter",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int line_numbers_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: line-numbers <on|off|relative|absolute|status>",
        "Examples:",
        "  line-numbers on        Enable absolute line numbers in multiline input",
        "  line-numbers relative  Enable relative line numbers in multiline input",
        "  line-numbers off       Disable line numbers in multiline input",
        "  line-numbers status    Show the current setting"};

    const auto describe_status = [] {
        if (!ic_line_numbers_are_enabled()) {
            return std::string("Line numbers are currently disabled.");
        }
        if (ic_line_numbers_are_relative()) {
            return std::string("Line numbers are currently enabled (relative numbering).");
        }
        return std::string("Line numbers are currently enabled (absolute numbering).");
    };

    if (args.size() == 1) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "line-numbers", "Missing option argument", usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "line-numbers", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    enum class LineNumbersMode : std::uint8_t {
        Status,
        Off,
        Relative,
        Absolute
    };

    auto parse_line_numbers_mode = [&](const std::string& value) -> std::optional<LineNumbersMode> {
        if (matches_token(value, {"status", "--status"})) {
            return LineNumbersMode::Status;
        }
        if (matches_token(value, {"off", "disable", "disabled", "false", "0", "--disable"})) {
            return LineNumbersMode::Off;
        }
        if (matches_token(value, {"relative", "rel", "--relative"})) {
            return LineNumbersMode::Relative;
        }
        if (matches_token(value, {"absolute", "abs", "--absolute"}) ||
            matches_token(value, {"on", "enable", "enabled", "true", "1", "--enable"})) {
            return LineNumbersMode::Absolute;
        }
        return std::nullopt;
    };

    auto mode = parse_line_numbers_mode(normalized);
    if (!mode.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, "line-numbers", "Unknown option '" + option + "'",
                     usage_lines});
        return 1;
    }

    if (*mode == LineNumbersMode::Status) {
        if (!cjsh_env::startup_active()) {
            std::cout << describe_status() << '\n';
        }
        return 0;
    }

    const bool was_enabled = ic_line_numbers_are_enabled();
    const bool was_relative = ic_line_numbers_are_relative();
    bool changed = false;

    switch (*mode) {
        case LineNumbersMode::Off:
            (void)ic_enable_line_numbers(false);
            changed = (was_enabled || was_relative);
            break;
        case LineNumbersMode::Relative:
            (void)ic_enable_relative_line_numbers(true);
            changed = (!was_enabled || !was_relative);
            break;
        case LineNumbersMode::Absolute:
            (void)ic_enable_line_numbers(true);
            (void)ic_enable_relative_line_numbers(false);
            changed = (!was_enabled || was_relative);
            break;
        case LineNumbersMode::Status:
            break;
    }

    if (!cjsh_env::startup_active() && changed) {
        std::cout << describe_status() << '\n';
        std::string persist_token;
        if (!ic_line_numbers_are_enabled()) {
            persist_token = "off";
        } else if (ic_line_numbers_are_relative()) {
            persist_token = "relative";
        } else {
            persist_token = "absolute";
        }
        std::cout << "Add `cjshopt line-numbers " << persist_token
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int line_numbers_continuation_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: line-numbers-continuation <on|off|status>", "Examples:",
        "  line-numbers-continuation on       Keep line numbers when a continuation prompt is set",
        "  line-numbers-continuation off      Hide line numbers whenever a continuation prompt is "
        "set",
        "  line-numbers-continuation status   Show the current setting"};

    static const ToggleCommandConfig config{
        "line-numbers-continuation",
        usage_lines,
        [] { return ic_line_numbers_with_continuation_prompt_are_enabled(); },
        [](bool enable) { (void)ic_enable_line_numbers_with_continuation_prompt(enable); },
        "Line numbers with continuation prompts",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int line_numbers_replace_prompt_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: line-numbers-replace-prompt <on|off|status>", "Examples:",
        "  line-numbers-replace-prompt on      Replace the final prompt line with line numbers",
        "  line-numbers-replace-prompt off     Keep the final prompt line visible",
        "  line-numbers-replace-prompt status  Show the current setting"};

    static const ToggleCommandConfig config{
        "line-numbers-replace-prompt",
        usage_lines,
        [] { return ic_line_number_prompt_replacement_is_enabled(); },
        [](bool enable) { (void)ic_enable_line_number_prompt_replacement(enable); },
        "Line number prompt replacement",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int hint_delay_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: hint-delay <milliseconds|status>",
        "Examples:",
        "  hint-delay 100    Set hint delay to 100 milliseconds",
        "  hint-delay 0      Show hints immediately",
        "  hint-delay status Show the current delay setting",
        "Values above 5000 milliseconds are clamped to 5000."};

    if (args.size() == 1) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "hint-delay", "Missing delay value", usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "hint-delay", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    std::string normalized = normalize_option(option);

    if (parse_status_query(normalized) == StatusQuery::Status) {
        if (!cjsh_env::startup_active()) {
            std::cout << "Hint delay is currently " << ic_get_hint_delay() << " milliseconds.\n";
        }
        return 0;
    }

    long delay_ms = 0;
    if (!numeric_utils::parse_long_strict(option, delay_ms) || delay_ms < 0) {
        print_error({ErrorType::INVALID_ARGUMENT, "hint-delay",
                     "Invalid delay value '" + option + "' (expected a non-negative integer)",
                     usage_lines});
        return 1;
    }

    (void)ic_set_hint_delay(delay_ms);
    const long applied_delay = ic_get_hint_delay();

    if (!cjsh_env::startup_active()) {
        if (applied_delay != delay_ms) {
            std::cout << "Hint delay exceeds the supported maximum; using " << applied_delay
                      << " milliseconds instead.\n";
        }
        std::cout << "Hint delay set to " << applied_delay << " milliseconds.\n";
        std::cout << "Add `cjshopt hint-delay " << applied_delay
                  << "` to your ~/.cjshrc to persist this change.\n";
    }
    return 0;
}

int idle_timeout_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: idle-timeout <seconds|off|status>",
        "Examples:", "  idle-timeout 120     Run idle hooks after 120 seconds without input",
        "  idle-timeout off     Disable idle hooks",
        "  idle-timeout status  Show the current timeout"};

    if (args.size() == 1) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "idle-timeout", "Missing timeout value", usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "idle-timeout", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string normalized = normalize_option(args[1]);
    if (parse_status_query(normalized) == StatusQuery::Status) {
        if (!cjsh_env::startup_active()) {
            if (config::idle_timeout_seconds <= 0) {
                std::cout << "Idle hooks are disabled.\n";
            } else {
                std::cout << "Idle hooks run after " << config::idle_timeout_seconds
                          << (config::idle_timeout_seconds == 1 ? " second" : " seconds")
                          << " without terminal input.\n";
            }
        }
        return 0;
    }

    long timeout_seconds = 0;
    if (normalized != "off" && normalized != "disable" && normalized != "disabled" &&
        normalized != "0") {
        if (!numeric_utils::parse_long_strict(args[1], timeout_seconds) || timeout_seconds < 1 ||
            timeout_seconds > LONG_MAX / 1000) {
            print_error({ErrorType::INVALID_ARGUMENT, "idle-timeout",
                         "Timeout must be a positive whole number of seconds or 'off'",
                         usage_lines});
            return 1;
        }
    }

    config::idle_timeout_seconds = timeout_seconds;
    (void)ic_set_idle_timeout(timeout_seconds * 1000);

    if (!cjsh_env::startup_active()) {
        if (timeout_seconds == 0) {
            std::cout << "Idle hooks disabled.\n";
        } else {
            std::cout << "Idle hooks will run after " << timeout_seconds
                      << (timeout_seconds == 1 ? " second" : " seconds")
                      << " without terminal input.\n";
        }
        std::cout << "Add `cjshopt idle-timeout "
                  << (timeout_seconds == 0 ? "off" : std::to_string(timeout_seconds))
                  << "` to your ~/.cjshrc to persist this change.\n";
    }
    return 0;
}

int multiline_start_lines_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: multiline-start-lines <count|status>",
        "Examples:", "  multiline-start-lines 1    Start editing on the first prompt line",
        "  multiline-start-lines 2    Start with two prompt lines (cursor on line 2)",
        "  multiline-start-lines status   Show the current setting"};

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-start-lines", "Missing line count",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-start-lines",
                     "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    std::string normalized = normalize_option(option);

    if (parse_status_query(normalized) == StatusQuery::Status) {
        if (!cjsh_env::startup_active()) {
            const size_t current = ic_get_multiline_start_line_count();
            std::cout << "Multiline prompts currently start with " << current << " line"
                      << (current == 1 ? "" : "s") << ".\n";
        }
        return 0;
    }

    long parsed = 0;
    if (!numeric_utils::parse_long_strict(option, parsed) || parsed < 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-start-lines",
                     "Invalid line count '" + option + "' (expected a positive integer)",
                     usage_lines});
        return 1;
    }
    const size_t requested = static_cast<size_t>(parsed);

    (void)ic_set_multiline_start_line_count(requested);
    const size_t applied = ic_get_multiline_start_line_count();

    if (!cjsh_env::startup_active()) {
        if (applied != requested) {
            std::cout << "Line count exceeds the supported maximum; using " << applied
                      << " instead.\n";
        }
        std::cout << "Multiline prompts will now start with " << applied << " line"
                  << (applied == 1 ? "" : "s") << ".\n";
        std::cout << "Add `cjshopt multiline-start-lines " << applied
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

namespace {
int max_lines_command(const std::vector<std::string>& args, const std::string& command,
                      const std::string& label, size_t (*get_line_count)(),
                      size_t (*set_line_count)(size_t),
                      const std::vector<std::string>& usage_lines) {
    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, command, "Missing line count", usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, command, "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    if (parse_status_query(normalized) == StatusQuery::Status) {
        if (!cjsh_env::startup_active()) {
            const size_t current = get_line_count();
            std::cout << label << " currently shows up to " << current << " line"
                      << (current == 1 ? "" : "s") << ".\n";
        }
        return 0;
    }

    if (option.empty() || !std::all_of(option.begin(), option.end(),
                                       [](unsigned char c) { return std::isdigit(c) != 0; })) {
        print_error({ErrorType::INVALID_ARGUMENT, command,
                     "Invalid line count '" + option + "' (expected a positive integer)",
                     usage_lines});
        return 1;
    }

    size_t requested = 0;
    try {
        requested = static_cast<size_t>(std::stoul(option));
    } catch (...) {
        print_error({ErrorType::INVALID_ARGUMENT, command,
                     "Invalid line count '" + option + "' (expected a positive integer)",
                     usage_lines});
        return 1;
    }

    if (requested == 0) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, command, "Line count must be at least 1", usage_lines});
        return 1;
    }

    (void)set_line_count(requested);
    const size_t applied = get_line_count();

    if (!cjsh_env::startup_active()) {
        if (applied != requested) {
            std::cout << "Line count exceeds the supported maximum; using " << applied
                      << " instead.\n";
        }
        std::cout << label << " will now show up to " << applied << " line"
                  << (applied == 1 ? "" : "s") << ".\n";
        std::cout << "Add `cjshopt " << command << " " << applied
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}
}  // namespace

int multiline_max_lines_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: multiline-max-lines <count|status>",
        "Examples:", "  multiline-max-lines 15       Show up to 15 multiline input rows",
        "  multiline-max-lines 5        Use a compact five-row viewport",
        "  multiline-max-lines status   Show the current setting"};
    return max_lines_command(args, "multiline-max-lines", "Multiline input",
                             ic_get_multiline_max_line_count, ic_set_multiline_max_line_count,
                             usage_lines);
}

int menu_max_lines_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: menu-max-lines <count|status>",
        "Limit content rows in completion, history, command palette, and custom menus.",
        "Examples:",
        "  menu-max-lines 50       Restore the default menu height",
        "  menu-max-lines 8        Show up to eight menu content rows",
        "  menu-max-lines status   Show the current setting"};
    return max_lines_command(args, "menu-max-lines", "Menu content", ic_get_menu_max_line_count,
                             ic_set_menu_max_line_count, usage_lines);
}

int multiline_bottom_lines_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: multiline-bottom-lines <count|status>",
        "Examples:", "  multiline-bottom-lines 3        Keep a three-row cursor margin",
        "  multiline-bottom-lines 0        Disable the cursor margin",
        "  multiline-bottom-lines status   Show the current setting"};

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-bottom-lines", "Missing line count",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-bottom-lines",
                     "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    if (parse_status_query(normalized) == StatusQuery::Status) {
        if (!cjsh_env::startup_active()) {
            const size_t current = ic_get_multiline_bottom_line_count();
            std::cout << "Multiline input currently uses a cursor margin of up to " << current
                      << " content line" << (current == 1 ? "" : "s") << ".\n";
        }
        return 0;
    }

    if (option.empty() || !std::all_of(option.begin(), option.end(),
                                       [](unsigned char c) { return std::isdigit(c) != 0; })) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-bottom-lines",
                     "Invalid line count '" + option + "' (expected a non-negative integer)",
                     usage_lines});
        return 1;
    }

    size_t requested = 0;
    try {
        requested = static_cast<size_t>(std::stoul(option));
    } catch (...) {
        print_error({ErrorType::INVALID_ARGUMENT, "multiline-bottom-lines",
                     "Invalid line count '" + option + "' (expected a non-negative integer)",
                     usage_lines});
        return 1;
    }

    (void)ic_set_multiline_bottom_line_count(requested);
    const size_t applied = ic_get_multiline_bottom_line_count();

    if (!cjsh_env::startup_active()) {
        if (applied != requested) {
            std::cout << "Line count exceeds the supported maximum; using " << applied
                      << " instead.\n";
        }
        std::cout << "Multiline input will now use a cursor margin of up to " << applied
                  << " content line" << (applied == 1 ? "" : "s") << ".\n";
        std::cout << "Add `cjshopt multiline-bottom-lines " << applied
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int completion_preview_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-preview <on|off|status>",
        "Examples:", "  completion-preview on      Enable completion preview",
        "  completion-preview off     Disable completion preview",
        "  completion-preview status  Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-preview",
        usage_lines,
        [] { return ic_completion_preview_is_enabled(); },
        [](bool enable) { (void)ic_enable_completion_preview(enable); },
        "Completion preview",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int completion_auto_menu_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-auto-menu <on|off|status>",
        "Examples:", "  completion-auto-menu on      Show completions while typing; Tab activates",
        "  completion-auto-menu off     Open completions only on request (default)",
        "  completion-auto-menu status  Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-auto-menu",
        usage_lines,
        [] { return ic_completion_auto_menu_is_enabled(); },
        [](bool enable) { (void)ic_enable_completion_auto_menu(enable); },
        "Automatic completion menu",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int completion_click_accept_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: completion-click-accept <on|off|status>",
        "Examples:", "  completion-click-accept on       Always accept completions when clicked",
        "  completion-click-accept off      Click selects completions without accepting",
        "  completion-click-accept status   Show the current setting"};

    static const ToggleCommandConfig config{
        "completion-click-accept",
        usage_lines,
        [] { return ic_completion_click_accept_is_enabled(); },
        [](bool enable) { (void)ic_enable_completion_click_accept(enable); },
        "Completion click-to-accept",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int menu_highlighting_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: menu-highlighting <none|single|all|reverse|status>",
        "Examples:",
        "  menu-highlighting none    Keep completion/history menu items unhighlighted",
        "  menu-highlighting single  Highlight only the selected menu item",
        "  menu-highlighting all     Highlight every rendered menu item",
        "  menu-highlighting reverse Highlight every rendered item except the selected one",
        "  menu-highlighting status  Show the current mode"};

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "menu-highlighting", "Missing option argument",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        if (!cjsh_env::startup_active()) {
            std::cout << "Current: " << describe_menu_highlight_mode(ic_get_menu_highlight_mode())
                      << '\n';
        }
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "menu-highlighting",
                     "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);
    if (matches_token(normalized, {"status", "--status"})) {
        if (!cjsh_env::startup_active()) {
            std::cout << "Menu highlighting mode is currently "
                      << describe_menu_highlight_mode(ic_get_menu_highlight_mode()) << ".\n";
        }
        return 0;
    }

    std::optional<ic_menu_highlight_mode_t> requested = parse_menu_highlight_mode(normalized);
    if (!requested.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, "menu-highlighting",
                     "Unknown option '" + option + "'", usage_lines});
        return 1;
    }

    const ic_menu_highlight_mode_t previous = ic_get_menu_highlight_mode();
    if (previous == *requested) {
        return 0;
    }

    (void)ic_set_menu_highlight_mode(*requested);

    if (!cjsh_env::startup_active()) {
        std::cout << "Menu highlighting set to " << canonical_menu_highlight_token(*requested)
                  << ".\n";
        std::cout << "Add `cjshopt menu-highlighting " << canonical_menu_highlight_token(*requested)
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int visible_whitespace_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: visible-whitespace <on|off|status>",
        "Examples:", "  visible-whitespace on      Show whitespace characters while editing",
        "  visible-whitespace off     Hide whitespace characters while editing",
        "  visible-whitespace status  Show the current setting"};

    static const ToggleCommandConfig config{
        "visible-whitespace",
        usage_lines,
        [] { return ic_visible_whitespace_is_enabled(); },
        [](bool enable) { (void)ic_enable_visible_whitespace(enable); },
        "Visible whitespace characters",
        true,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int line_wrap_marker_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: line-wrap-marker <marker|status>",
        "The marker must be one printable Unicode character, or '' to disable it.",
        "Examples:",
        "  line-wrap-marker '>'     Set the symbol at wrapped line ends",
        "  line-wrap-marker ''      Hide the symbol at wrapped line ends",
        "  line-wrap-marker status  Show the current marker"};

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "line-wrap-marker", "Missing marker argument",
                     usage_lines});
        return 1;
    }
    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }
    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "line-wrap-marker", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const bool status = matches_token(normalize_option(args[1]), {"status", "--status"});
    if (!status && !ic_set_line_wrap_marker(args[1].c_str())) {
        print_error({ErrorType::INVALID_ARGUMENT, "line-wrap-marker",
                     "Marker must be empty or one printable Unicode character with positive "
                     "display width",
                     usage_lines});
        return 1;
    }

    if (!cjsh_env::startup_active()) {
        const char* current = ic_get_line_wrap_marker();
        const std::string marker = (current == nullptr ? "" : current);
        const std::string quoted = (marker == "'" ? "\"'\"" : "'" + marker + "'");
        std::cout << "Line wrap marker " << (status ? "is currently " : "set to ") << quoted
                  << (marker.empty() ? " (disabled).\n" : ".\n");
        if (!status) {
            std::cout << "Add `cjshopt line-wrap-marker " << quoted
                      << "` to your ~/.cjshrc to persist this change.\n";
        }
    }
    return 0;
}

int hint_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: hint <on|off|status>", "Examples:", "  hint on      Enable inline hints",
        "  hint off     Disable inline hints", "  hint status  Show the current setting"};

    static const ToggleCommandConfig config{
        "hint",
        usage_lines,
        [] { return ic_hint_is_enabled(); },
        [](bool enable) { (void)ic_enable_hint(enable); },
        "Inline hints",
        true,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int multiline_indent_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: multiline-indent <on|off|status>",
        "Examples:", "  multiline-indent on      Enable automatic indentation in multiline",
        "  multiline-indent off     Disable automatic indentation in multiline",
        "  multiline-indent status  Show the current setting"};

    static const ToggleCommandConfig config{
        "multiline-indent",
        usage_lines,
        [] { return ic_multiline_indent_is_enabled(); },
        [](bool enable) { (void)ic_enable_multiline_indent(enable); },
        "Multiline auto-indent",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int multiline_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: multiline <on|off|status>",
        "Examples:", "  multiline on      Enable multiline input",
        "  multiline off     Disable multiline input",
        "  multiline status  Show the current setting"};

    static const ToggleCommandConfig config{
        "multiline",
        usage_lines,
        [] { return ic_multiline_is_enabled(); },
        [](bool enable) { (void)ic_enable_multiline(enable); },
        "Multiline input",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int inline_help_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: inline-help <on|off|status>",
        "Examples:", "  inline-help on      Enable inline help messages",
        "  inline-help off     Disable inline help messages",
        "  inline-help status  Show the current setting"};

    static const ToggleCommandConfig config{
        "inline-help",
        usage_lines,
        [] { return ic_inline_help_is_enabled(); },
        [](bool enable) { (void)ic_enable_inline_help(enable); },
        "Inline help messages",
        true,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int status_hints_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: status-hints <off|normal|transient|persistent|status>",
        "Examples:",
        "  status-hints off          Never display the underlined status hints",
        "  status-hints normal       Only show hints when the buffer and status are blank "
        "(default)",
        "  status-hints transient    Show hints when the status line is empty",
        "  status-hints persistent   Always prepend hints above other status messages",
        "  status-hints status       Show the current mode"};

    ensure_status_hint_preference_initialized();

    if (args.size() == 1) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "status-hints", "Missing option argument", usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        if (!cjsh_env::startup_active()) {
            std::cout << "Current: " << describe_status_hint_mode(ic_get_status_hint_mode())
                      << '\n';
        }
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "status-hints", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    enum class StatusHintsMode : std::uint8_t {
        Status,
        Off,
        Normal,
        Transient,
        Persistent
    };

    auto parse_status_hints_mode = [&](const std::string& value) -> std::optional<StatusHintsMode> {
        if (matches_token(value, {"status", "--status"})) {
            return StatusHintsMode::Status;
        }
        if (matches_token(value, {"off", "disable", "disabled", "never", "hidden", "--disable"})) {
            return StatusHintsMode::Off;
        }
        if (matches_token(value, {"normal", "minimal", "empty-only", "default"})) {
            return StatusHintsMode::Normal;
        }
        if (matches_token(value, {"transient", "auto"})) {
            return StatusHintsMode::Transient;
        }
        if (matches_token(value, {"persistent", "always", "always-on", "on"})) {
            return StatusHintsMode::Persistent;
        }
        return std::nullopt;
    };

    auto mode = parse_status_hints_mode(normalized);
    if (!mode.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, "status-hints", "Unknown option '" + option + "'",
                     usage_lines});
        return 1;
    }

    if (*mode == StatusHintsMode::Status) {
        if (!cjsh_env::startup_active()) {
            if (config::status_line_enabled) {
                std::cout << "Status hints are currently "
                          << describe_status_hint_mode(g_status_hint_preference) << ".\n";
            } else {
                std::cout << "Status hints preference is "
                          << describe_status_hint_mode(g_status_hint_preference)
                          << ", but the status line toggle is off so the banner stays hidden.\n";
            }
        }
        return 0;
    }

    ic_status_hint_mode_t target = IC_STATUS_HINT_NORMAL;
    switch (*mode) {
        case StatusHintsMode::Off:
            target = IC_STATUS_HINT_OFF;
            break;
        case StatusHintsMode::Normal:
            target = IC_STATUS_HINT_NORMAL;
            break;
        case StatusHintsMode::Transient:
            target = IC_STATUS_HINT_TRANSIENT;
            break;
        case StatusHintsMode::Persistent:
            target = IC_STATUS_HINT_PERSISTENT;
            break;
        case StatusHintsMode::Status:
            break;
    }

    const bool preference_changed = (g_status_hint_preference != target);
    g_status_hint_preference = target;

    if (!preference_changed) {
        if (!cjsh_env::startup_active() && !config::status_line_enabled) {
            std::cout << "Status hints stay hidden because the status line toggle is off.\n";
        }
        return 0;
    }

    apply_effective_status_hint_mode();

    if (!cjsh_env::startup_active()) {
        if (config::status_line_enabled) {
            std::cout << "Status hints set to " << describe_status_hint_mode(target) << ".\n";
            std::cout << "Add `cjshopt status-hints " << canonical_status_hint_token(target)
                      << "` to your ~/.cjshrc to persist this change.\n";
        } else {
            std::cout << "Stored status hint mode set to " << describe_status_hint_mode(target)
                      << ", but the status line toggle is off so nothing is shown.\n";
            std::cout << "Re-enable it with `cjshopt status-line on` to display the banner.\n";
        }
    }

    return 0;
}

int status_line_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: status-line <on|off|status>",
        "Examples:", "  status-line on      Show the status area below the prompt",
        "  status-line off     Hide the status area entirely",
        "  status-line status  Show the current setting"};

    static const ToggleCommandConfig config{
        "status-line",
        usage_lines,
        [] { return config::status_line_enabled; },
        [](bool enable) {
            config::status_line_enabled = enable;
            apply_effective_status_hint_mode();
        },
        "Status line",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int status_reporting_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: status-reporting <on|off|status>",
        "Examples:", "  status-reporting on      Show cjsh validation output in the status row",
        "  status-reporting off     Hide validation and error reporting",
        "  status-reporting status  Show the current setting"};

    static const ToggleCommandConfig command_config{
        "status-reporting",
        usage_lines,
        [] { return config::status_reporting_enabled; },
        [](bool enable) { config::status_reporting_enabled = enable; },
        "Status reporting",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(command_config, args);
}

int status_line_callback_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: status-line-callback <function_name|off|status>", "Examples:",
        "  status-line-callback my_status_banner  Run my_status_banner before drawing the status "
        "row",
        "  status-line-callback off               Disable custom status-line callback output",
        "  status-line-callback status            Show the current callback setting"};

    auto print_current_state = [] {
        const std::string current_callback = status_line::get_user_status_callback_function();
        if (current_callback.empty()) {
            std::cout << "Status-line callback is currently disabled.\n";
            return;
        }

        if (config::status_line_enabled) {
            std::cout << "Status-line callback function is currently '" << current_callback
                      << "'.\n";
            return;
        }

        std::cout << "Status-line callback function is currently '" << current_callback
                  << "', but the status-line toggle is off so callback output is hidden.\n";
    };

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "status-line-callback", "Missing option argument",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        if (!cjsh_env::startup_active()) {
            print_current_state();
        }
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "status-line-callback",
                     "Too many arguments provided", usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    if (matches_token(normalized, {"status", "--status"})) {
        if (!cjsh_env::startup_active()) {
            print_current_state();
        }
        return 0;
    }

    if (matches_token(normalized, {"off", "disable", "disabled", "none", "clear", "--disable"})) {
        const bool had_callback = !status_line::get_user_status_callback_function().empty();
        status_line::clear_user_status_callback_function();

        if (!cjsh_env::startup_active() && had_callback) {
            std::cout << "Status-line callback disabled.\n";
            std::cout << "Add `cjshopt status-line-callback off` to your ~/.cjshrc to persist this "
                         "change.\n";
        }
        return 0;
    }

    if (!is_valid_identifier(option)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "status-line-callback",
                     "Invalid function name '" + option + "'",
                     {"Function names must start with a letter or underscore and contain only "
                      "letters, digits, and underscores."}});
        return 1;
    }

    status_line::set_user_status_callback_function(option);

    bool function_exists = false;
    if (g_shell != nullptr) {
        if (ShellScriptInterpreter* interpreter = g_shell->get_shell_script_interpreter();
            interpreter != nullptr) {
            function_exists = interpreter->has_function(option);
        }
    }

    if (!cjsh_env::startup_active()) {
        if (config::status_line_enabled) {
            std::cout << "Status-line callback set to '" << option << "'.\n";
        } else {
            std::cout << "Status-line callback set to '" << option
                      << "', but the status-line toggle is off so callback output stays hidden.\n";
            std::cout << "Re-enable it with `cjshopt status-line on` to display callback output.\n";
        }

        if (!function_exists) {
            std::cout << "Function '" << option
                      << "' is not defined yet; define it in this session or in ~/.cjshrc.\n";
        }

        std::cout << "Add `cjshopt status-line-callback " << option
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int mouse_clicking_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: mouse-clicking <all-off|off|simple|smart|status>",
        "Examples:",
        "  mouse-clicking all-off   Never capture mouse events, including in menus",
        "  mouse-clicking off       Capture mouse events only in interactive menus",
        "  mouse-clicking simple    Start with mouse capture enabled; toggle manually",
        "  mouse-clicking smart     Start enabled with automatic suspend/resume",
        "  mouse-clicking status    Show the current mode"};

    const auto describe_mode = [](ic_mouse_clicking_mode_t mode) -> const char* {
        switch (mode) {
            case IC_MOUSE_CLICKING_DISABLED:
                return "all mouse clicking disabled";
            case IC_MOUSE_CLICKING_MENU_ONLY:
                return "editing capture off; interactive menus only";
            case IC_MOUSE_CLICKING_SIMPLE:
                return "manual toggle only";
            case IC_MOUSE_CLICKING_SMART:
                return "auto suspend/resume";
            default:
                return "auto suspend/resume";
        }
    };

    const auto canonical_mode_token = [](ic_mouse_clicking_mode_t mode) -> const char* {
        switch (mode) {
            case IC_MOUSE_CLICKING_DISABLED:
                return "all-off";
            case IC_MOUSE_CLICKING_MENU_ONLY:
                return "off";
            case IC_MOUSE_CLICKING_SIMPLE:
                return "simple";
            case IC_MOUSE_CLICKING_SMART:
                return "smart";
            default:
                return "smart";
        }
    };

    const auto parse_mode =
        [](const std::string& normalized) -> std::optional<ic_mouse_clicking_mode_t> {
        if (matches_token(normalized, {"all-off", "alloff", "disabled", "none"})) {
            return IC_MOUSE_CLICKING_DISABLED;
        }
        if (matches_token(normalized,
                          {"off", "disable", "false", "0", "--disable", "menu-only", "menus"})) {
            return IC_MOUSE_CLICKING_MENU_ONLY;
        }
        if (matches_token(normalized,
                          {"simple", "on", "enable", "enabled", "true", "1", "--enable"})) {
            return IC_MOUSE_CLICKING_SIMPLE;
        }
        if (matches_token(normalized, {"smart", "auto", "automatic"})) {
            return IC_MOUSE_CLICKING_SMART;
        }
        return std::nullopt;
    };

    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "mouse-clicking", "Missing option argument",
                     usage_lines});
        return 1;
    }

    if (builtin_handle_help_with_startup_guard(args, usage_lines)) {
        return 0;
    }

    if (args.size() != 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "mouse-clicking", "Too many arguments provided",
                     usage_lines});
        return 1;
    }

    const std::string& option = args[1];
    const std::string normalized = normalize_option(option);

    if (matches_token(normalized, {"status", "--status"})) {
        if (!cjsh_env::startup_active()) {
            ic_mouse_clicking_mode_t mode = ic_get_mouse_clicking_mode();
            std::cout << "Mouse clicking mode is currently " << canonical_mode_token(mode) << " ("
                      << describe_mode(mode) << ").\n";
        }
        return 0;
    }

    std::optional<ic_mouse_clicking_mode_t> requested_mode = parse_mode(normalized);
    if (!requested_mode.has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT, "mouse-clicking",
                     "Unknown option '" + option + "'", usage_lines});
        return 1;
    }

    ic_mouse_clicking_mode_t current_mode = ic_get_mouse_clicking_mode();
    if (current_mode == *requested_mode) {
        return 0;
    }

    (void)ic_set_mouse_clicking_mode(*requested_mode);

    if (!cjsh_env::startup_active()) {
        std::cout << "Mouse clicking mode set to " << canonical_mode_token(*requested_mode) << " ("
                  << describe_mode(*requested_mode) << ").\n";
        std::cout << "Add `cjshopt mouse-clicking " << canonical_mode_token(*requested_mode)
                  << "` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int mouse_clicking_status_line_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: mouse-clicking-status-line <on|off|status>", "Examples:",
        "  mouse-clicking-status-line on      Show the mouse-clicking status indicator",
        "  mouse-clicking-status-line off     Hide the mouse-clicking status indicator",
        "  mouse-clicking-status-line status  Show the current setting"};

    static const ToggleCommandConfig config{
        "mouse-clicking-status-line",
        usage_lines,
        [] { return ic_mouse_reporting_status_line_is_enabled(); },
        [](bool enable) { (void)ic_enable_mouse_reporting_status_line(enable); },
        "Mouse clicking status indicator",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int auto_tab_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: auto-tab <on|off|status>",
        "Examples:", "  auto-tab on      Enable automatic tab completion",
        "  auto-tab off     Disable automatic tab completion",
        "  auto-tab status  Show the current setting"};

    static const ToggleCommandConfig config{
        "auto-tab",
        usage_lines,
        [] { return ic_auto_tab_is_enabled(); },
        [](bool enable) { (void)ic_enable_auto_tab(enable); },
        "Automatic tab completion",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int prompt_newline_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: prompt-newline <on|off|status>",
        "Examples:", "  prompt-newline on      Add a newline after each command",
        "  prompt-newline off     Disable newlines after commands",
        "  prompt-newline status  Show the current setting"};

    static const ToggleCommandConfig config{
        "prompt-newline",
        usage_lines,
        [] { return config::newline_after_execution; },
        [](bool enable) { config::newline_after_execution = enable; },
        "Post-execution newline",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}

int right_prompt_follow_cursor_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> usage_lines = {
        "Usage: right-prompt-follow-cursor <on|off|status>", "Examples:",
        "  right-prompt-follow-cursor on      Move the inline right prompt with the cursor",
        "  right-prompt-follow-cursor off     Pin the inline right prompt to the first row",
        "  right-prompt-follow-cursor status  Show the current setting"};

    static const ToggleCommandConfig config{
        "right-prompt-follow-cursor",
        usage_lines,
        [] { return ic_inline_right_prompt_follows_cursor(); },
        [](bool enable) { (void)ic_enable_inline_right_prompt_cursor_follow(enable); },
        "Right prompt cursor tracking",
        false,
        "Add `cjshopt {command} {state}` to your ~/.cjshrc to persist this change.\n",
        {},
        {}};

    return handle_toggle_command(config, args);
}
