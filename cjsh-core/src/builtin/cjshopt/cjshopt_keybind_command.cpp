/*
  cjshopt_keybind_command.cpp

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

#include "cjshopt_registry.h"

#include "cjshopt_command.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agent_mode.h"
#include "browser.h"
#include "error_out.h"
#include "isocline.h"
#include "keybindings.h"
#include "shell_env.h"
#include "string_utils.h"

namespace {
struct KeyBindingDefault {
    ic_key_action_t action;
    const char* canonical_name;
};

const std::vector<KeyBindingDefault>& key_binding_defaults() {
    static const std::vector<KeyBindingDefault> kDefaults = {
        {IC_KEY_ACTION_CURSOR_LEFT, "cursor-left"},
        {IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE, "cursor-right"},
        {IC_KEY_ACTION_CURSOR_UP, "cursor-up"},
        {IC_KEY_ACTION_CURSOR_DOWN, "cursor-down"},
        {IC_KEY_ACTION_CURSOR_WORD_PREV, "cursor-word-prev"},
        {IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE, "cursor-word-next"},
        {IC_KEY_ACTION_CURSOR_LINE_START, "cursor-line-start"},
        {IC_KEY_ACTION_CURSOR_LINE_END, "cursor-line-end"},
        {IC_KEY_ACTION_CURSOR_INPUT_START, "cursor-input-start"},
        {IC_KEY_ACTION_CURSOR_INPUT_END, "cursor-input-end"},
        {IC_KEY_ACTION_CURSOR_MATCH_BRACE, "cursor-match-brace"},
        {IC_KEY_ACTION_HISTORY_PREV, "history-prev"},
        {IC_KEY_ACTION_HISTORY_NEXT, "history-next"},
        {IC_KEY_ACTION_HISTORY_SEARCH, "history-search"},
        {IC_KEY_ACTION_COMMAND_PALETTE, "command-palette"},
        {IC_KEY_ACTION_DELETE_FORWARD, "delete-forward"},
        {IC_KEY_ACTION_DELETE_BACKWARD, "delete-backward"},
        {IC_KEY_ACTION_DELETE_WORD_START_WS, "delete-word-start-ws"},
        {IC_KEY_ACTION_DELETE_WORD_START, "delete-word-start"},
        {IC_KEY_ACTION_DELETE_WORD_END, "delete-word-end"},
        {IC_KEY_ACTION_DELETE_LINE_START, "delete-line-start"},
        {IC_KEY_ACTION_DELETE_LINE_END, "delete-line-end"},
        {IC_KEY_ACTION_TRANSPOSE_CHARS, "transpose-chars"},
        {IC_KEY_ACTION_YANK_LAST_ARG, "yank-last-arg"},
        {IC_KEY_ACTION_CLEAR_SCREEN, "clear-screen"},
        {IC_KEY_ACTION_UNDO, "undo"},
        {IC_KEY_ACTION_REDO, "redo"},
        {IC_KEY_ACTION_COMPLETE, "complete"},
        {IC_KEY_ACTION_INSERT_NEWLINE, "insert-newline"},
    };
    return kDefaults;
}

const std::vector<std::string>& keybind_usage_lines() {
    static const std::vector<std::string> kUsage = {
        cjshopt_subcommand_usage("keybind"),
        "",
        "Changes apply immediately in the current shell.",
        "Add the same command to ~/.cjshrc to persist it for future sessions.",
        "",
        "Subcommands:",
        "  list                            Show current default and custom key bindings",
        "  set <action> <keys...>          Replace bindings for an action",
        "  add <action> <keys...>          Add key bindings for an action",
        "  clear <keys...>                 Remove bindings for the specified key(s)",
        "  clear-action <action>           Remove all custom bindings for an action",
        "  reset                           Clear all custom key bindings and restore defaults",
        "  profile list                    List available key binding profiles",
        "  profile set <name>              Activate a key binding profile",
        "  ext <subcommand> [...]          Manage custom command and palette bindings",
        "",
        "Use 'cjshopt keybind --help' for detailed guidance.",
    };
    return kUsage;
}

std::optional<ic_key_action_t> parse_key_action_or_report(const std::string& action_name) {
    ic_key_action_t action = ic_key_action_from_name(action_name.c_str());
    if (action == IC_KEY_ACTION__MAX) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind", "Unknown action '" + action_name + "'",
                     keybind_usage_lines()});
        return std::nullopt;
    }
    return action;
}

std::vector<std::string> split_key_spec_string(const std::string& spec) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t end = spec.find('|', start);
        const size_t length = (end == std::string::npos ? spec.size() : end) - start;
        std::string token = spec.substr(start, length);
        token = string_utils::trim_ascii_whitespace_copy(token);
        if (!token.empty()) {
            result.push_back(token);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::string> parse_key_spec_arguments(const std::vector<std::string>& args,
                                                  size_t start_index) {
    std::vector<std::string> specs;
    for (size_t i = start_index; i < args.size(); ++i) {
        std::vector<std::string> tokens = split_key_spec_string(args[i]);
        (void)specs.insert(specs.end(), tokens.begin(), tokens.end());
    }
    return specs;
}

std::string join_specs(const std::vector<std::string>& specs) {
    if (specs.empty()) {
        return "(none)";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < specs.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << specs[i];
    }
    return oss.str();
}

std::string pipe_join_specs(const std::vector<std::string>& specs) {
    if (specs.empty()) {
        return "";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < specs.size(); ++i) {
        if (i != 0) {
            oss << '|';
        }
        oss << specs[i];
    }
    return oss.str();
}

std::vector<ic_key_binding_entry_t> collect_bindings() {
    size_t count = ic_list_key_bindings(nullptr, 0);
    std::vector<ic_key_binding_entry_t> entries(count);
    if (count > 0) {
        (void)ic_list_key_bindings(entries.data(), count);
    }
    return entries;
}

const KeyBindingDefault* find_default(ic_key_action_t action) {
    for (const auto& entry : key_binding_defaults()) {
        if (entry.action == action) {
            return &entry;
        }
    }
    return nullptr;
}

std::unordered_map<ic_key_action_t, std::vector<std::string>> group_bindings_by_action(
    const std::vector<ic_key_binding_entry_t>& entries) {
    std::unordered_map<ic_key_action_t, std::vector<std::string>> grouped;
    for (const auto& entry : entries) {
        char buffer[64];
        if (ic_format_key_spec(entry.key, buffer, sizeof(buffer))) {
            (void)grouped[entry.action].emplace_back(buffer);
        }
    }
    for (auto& pair : grouped) {
        auto& vec = pair.second;
        std::sort(vec.begin(), vec.end());
        (void)vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    }
    return grouped;
}

std::vector<std::string> available_action_names() {
    std::vector<std::string> names;
    names.reserve(key_binding_defaults().size() + 1);
    for (const auto& entry : key_binding_defaults()) {
        (void)names.emplace_back(entry.canonical_name);
    }
    (void)names.emplace_back("none");
    std::sort(names.begin(), names.end());
    (void)names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

void print_keybind_usage() {
    if (cjsh_env::startup_active()) {
        return;
    }
    for (const auto& line : keybind_usage_lines()) {
        std::cout << line << '\n';
    }
    std::cout << "Available actions: ";
    auto names = available_action_names();
    for (size_t i = 0; i < names.size(); ++i) {
        std::cout << names[i];
        if (i + 1 < names.size()) {
            std::cout << ", ";
        }
    }
    std::cout << '\n';
}

bool parse_key_specs_to_codes(const std::vector<std::string>& specs,
                              std::vector<std::pair<ic_keycode_t, std::string>>* out_codes,
                              std::string* invalid_spec) {
    out_codes->clear();
    std::unordered_set<ic_keycode_t> seen;
    for (const auto& spec : specs) {
        ic_keycode_t key_code = 0;
        if (!ic_parse_key_spec(spec.c_str(), &key_code)) {
            if (invalid_spec != nullptr) {
                *invalid_spec = spec;
            }
            return false;
        }
        if (seen.insert(key_code).second) {
            (void)out_codes->emplace_back(key_code, spec);
        }
    }
    return true;
}

bool parse_key_specs_from_args(const std::vector<std::string>& args, size_t start_index,
                               std::vector<std::pair<ic_keycode_t, std::string>>& parsed) {
    auto spec_args = parse_key_spec_arguments(args, start_index);
    if (spec_args.empty()) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                     "Provide at least one key specification", keybind_usage_lines()});
        return false;
    }

    std::string invalid_spec;
    if (!parse_key_specs_to_codes(spec_args, &parsed, &invalid_spec)) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                     "Invalid key specification '" + invalid_spec + "'", keybind_usage_lines()});
        return false;
    }
    return true;
}

bool parse_default_action_keys(ic_key_action_t action,
                               std::vector<std::pair<ic_keycode_t, std::string>>* out_codes) {
    out_codes->clear();
    const char* spec_string = ic_key_binding_profile_default_specs(action);
    if (spec_string == nullptr || spec_string[0] == '\0') {
        return true;
    }
    std::vector<std::string> tokens = split_key_spec_string(std::string(spec_string));
    if (tokens.empty()) {
        return true;
    }
    std::string invalid;
    return parse_key_specs_to_codes(tokens, out_codes, &invalid);
}

std::string canonical_action_name(ic_key_action_t action) {
    const KeyBindingDefault* info = find_default(action);
    if (info != nullptr) {
        return info->canonical_name;
    }
    const char* resolved = ic_key_action_name(action);
    if (resolved != nullptr) {
        return resolved;
    }
    return "(unknown)";
}

void remove_profile_defaults_from_group(
    std::unordered_map<ic_key_action_t, std::vector<std::string>>* grouped, ic_key_action_t action,
    const char* spec_string) {
    if (grouped == nullptr || spec_string == nullptr || spec_string[0] == '\0') {
        return;
    }
    auto it = grouped->find(action);
    if (it == grouped->end()) {
        return;
    }
    auto tokens = split_key_spec_string(std::string(spec_string));
    if (tokens.empty()) {
        return;
    }
    auto& specs = it->second;
    if (specs.empty()) {
        return;
    }
    for (const auto& token : tokens) {
        ic_keycode_t key = 0;
        if (!ic_parse_key_spec(token.c_str(), &key)) {
            continue;
        }
        char formatted[64];
        if (!ic_format_key_spec(key, formatted, sizeof(formatted))) {
            continue;
        }
        const std::string canonical = string_utils::to_lower_copy(formatted);
        for (size_t idx = 0; idx < specs.size();) {
            if (string_utils::to_lower_copy(specs[idx]) == canonical) {
                (void)specs.erase(specs.begin() + static_cast<std::ptrdiff_t>(idx));
            } else {
                ++idx;
            }
        }
    }
    if (specs.empty()) {
        (void)grouped->erase(it);
    }
}

int keybind_list_command() {
    auto entries = collect_bindings();
    auto grouped = group_bindings_by_action(entries);

    if (cjsh_env::startup_active()) {
        return 0;
    }

    const char* active_profile = ic_get_key_binding_profile();
    std::cout << "Active key binding profile: "
              << (active_profile != nullptr ? active_profile : "emacs") << "\n\n";

    size_t name_width = std::strlen("Action");
    for (const auto& entry : key_binding_defaults()) {
        name_width = std::max(name_width, std::strlen(entry.canonical_name));
    }
    for (const auto& pair : grouped) {
        if (find_default(pair.first) != nullptr) {
            continue;
        }
        const char* resolved = ic_key_action_name(pair.first);
        if (resolved != nullptr) {
            name_width = std::max(name_width, std::strlen(resolved));
        }
    }

    constexpr size_t default_column_width = 28;
    std::cout << std::left << std::setw(static_cast<int>(name_width) + 2) << "Action"
              << std::setw(static_cast<int>(default_column_width)) << "Default"
              << "Custom" << '\n';
    std::cout << std::string(name_width + 2 + default_column_width + 6, '-') << '\n';

    std::unordered_set<ic_key_action_t> printed;

    for (const auto& entry : key_binding_defaults()) {
        const char* spec_cstr = ic_key_binding_profile_default_specs(entry.action);
        std::string spec_string = (spec_cstr != nullptr ? spec_cstr : "");
        std::vector<std::string> default_specs = split_key_spec_string(spec_string);
        std::string default_display = join_specs(default_specs);

        remove_profile_defaults_from_group(&grouped, entry.action, spec_cstr);

        std::string custom_display = "(none)";
        auto it = grouped.find(entry.action);
        if (it != grouped.end()) {
            custom_display = join_specs(it->second);
            (void)printed.insert(entry.action);
        }

        std::cout << std::left << std::setw(static_cast<int>(name_width) + 2)
                  << entry.canonical_name << std::setw(static_cast<int>(default_column_width))
                  << default_display << custom_display << '\n';
    }

    for (const auto& pair : grouped) {
        if (printed.find(pair.first) != printed.end()) {
            continue;
        }
        std::string name = canonical_action_name(pair.first);
        std::cout << std::left << std::setw(static_cast<int>(name_width) + 2) << name
                  << std::setw(static_cast<int>(default_column_width)) << "(none)"
                  << join_specs(pair.second) << '\n';
    }

    if (entries.empty()) {
        std::cout << "\nNo custom key bindings are currently defined.\n";
        std::cout << "To customize key bindings, add 'cjshopt keybind ...' commands to your "
                     "~/.cjshrc file.\n";
    } else {
        std::cout << "\nCustom key bindings are defined in your configuration files.\n";
        std::cout << "To modify them, edit your ~/.cjshrc file.\n";
    }

    return 0;
}

int keybind_profile_list_command() {
    if (cjsh_env::startup_active()) {
        return 0;
    }

    size_t count = ic_list_key_binding_profiles(nullptr, 0);
    std::vector<ic_key_binding_profile_info_t> profiles(count);
    if (count > 0) {
        (void)ic_list_key_binding_profiles(profiles.data(), count);
    }

    const char* active = ic_get_key_binding_profile();
    std::string active_lower = (active != nullptr ? string_utils::to_lower_copy(active) : "");
    std::cout << "Available key binding profiles:\n";
    for (const auto& profile : profiles) {
        bool is_active =
            (profile.name != nullptr && string_utils::to_lower_copy(profile.name) == active_lower);
        std::cout << "  " << (is_active ? "* " : "  ")
                  << (profile.name != nullptr ? profile.name : "(unknown)");
        if (profile.description != nullptr && profile.description[0] != '\0') {
            std::cout << " - " << profile.description;
        }
        std::cout << '\n';
    }
    if (profiles.empty()) {
        std::cout << "  (no profiles available)\n";
    }
    return 0;
}

int keybind_profile_set_command(const std::vector<std::string>& args) {
    if (args.size() != 4) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind", "profile set requires a profile name",
                     keybind_usage_lines()});
        return 1;
    }

    const std::string& profile_name = args[3];
    if (!ic_set_key_binding_profile(profile_name.c_str())) {
        size_t count = ic_list_key_binding_profiles(nullptr, 0);
        std::vector<ic_key_binding_profile_info_t> profiles(count);
        if (count > 0) {
            (void)ic_list_key_binding_profiles(profiles.data(), count);
        }
        std::vector<std::string> names;
        names.reserve(profiles.size());
        for (const auto& profile : profiles) {
            if (profile.name != nullptr) {
                (void)names.emplace_back(profile.name);
            }
        }
        std::ostringstream oss;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                oss << ", ";
            }
            oss << names[i];
        }
        print_error(
            {ErrorType::INVALID_ARGUMENT,
             "keybind",
             "Unknown key binding profile '" + profile_name + "'",
             {"Available profiles: " + (names.empty() ? std::string("(none)") : oss.str())}});
        return 1;
    }

    if (!cjsh_env::startup_active()) {
        std::cout << "Key binding profile set to '" << profile_name << "'.\n";
        std::cout << "Add `cjshopt keybind profile set " << profile_name
                  << "` to your ~/.cjshrc to persist this change.\n";
    }
    agent_mode::apply_key_bindings();
    browser::apply_key_bindings();
    return 0;
}

int keybind_set_or_add_command(const std::vector<std::string>& args, bool replace_existing) {
    if (args.size() < 4) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                     std::string(replace_existing ? "set" : "add") +
                         " requires an action and at least one key specification",
                     keybind_usage_lines()});
        return 1;
    }

    const std::string& action_arg = args[2];
    auto parsed_action = parse_key_action_or_report(action_arg);
    if (!parsed_action.has_value()) {
        return 1;
    }
    ic_key_action_t action = *parsed_action;

    std::vector<std::pair<ic_keycode_t, std::string>> parsed;
    if (!parse_key_specs_from_args(args, 3, parsed)) {
        return 1;
    }

    std::unordered_set<ic_keycode_t> new_keys;
    new_keys.reserve(parsed.size());
    for (const auto& entry : parsed) {
        (void)new_keys.insert(entry.first);
    }

    std::vector<std::pair<ic_keycode_t, std::string>> default_keys;
    std::vector<std::pair<ic_keycode_t, std::string>> keys_to_suppress;
    if (replace_existing && parse_default_action_keys(action, &default_keys)) {
        for (const auto& def : default_keys) {
            if (new_keys.find(def.first) == new_keys.end()) {
                keys_to_suppress.push_back(def);
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> conflicts;
    for (const auto& entry : parsed) {
        ic_key_action_t existing_action;
        if (ic_get_key_binding(entry.first, &existing_action) && (existing_action != action)) {
            std::string existing_action_name = canonical_action_name(existing_action);
            conflicts.push_back({entry.second, existing_action_name});
        }
    }

    if (!conflicts.empty()) {
        for (const auto& conflict : conflicts) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         ErrorSeverity::WARNING,
                         "keybind",
                         "Key '" + conflict.first + "' was already bound to '" + conflict.second +
                             "' and will be overridden.",
                         {"Use 'cjshopt keybind list' to inspect current bindings."}});
        }
    }

    std::vector<ic_key_binding_entry_t> previous;
    if (replace_existing && action != IC_KEY_ACTION_NONE) {
        auto entries = collect_bindings();
        for (const auto& entry : entries) {
            if (entry.action == action) {
                previous.push_back(entry);
                (void)ic_clear_key_binding(entry.key);
            }
        }
    }

    for (const auto& entry : parsed) {
        (void)ic_clear_key_binding(entry.first);
    }

    for (const auto& entry : keys_to_suppress) {
        (void)ic_clear_key_binding(entry.first);
    }

    std::vector<ic_keycode_t> bound;
    bound.reserve(parsed.size());
    auto bind_or_rollback = [&](ic_keycode_t key, ic_key_action_t target_action,
                                const std::string& spec_for_error) {
        if (!ic_bind_key(key, target_action)) {
            for (const auto& k : bound) {
                (void)ic_clear_key_binding(k);
            }
            for (const auto& prev : previous) {
                (void)ic_bind_key(prev.key, prev.action);
            }
            print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                         "Failed to bind key specification '" + spec_for_error + "'",
                         keybind_usage_lines()});
            return false;
        }
        bound.push_back(key);
        return true;
    };

    for (const auto& entry : keys_to_suppress) {
        if (!bind_or_rollback(entry.first, IC_KEY_ACTION_NONE, entry.second)) {
            return 1;
        }
    }

    for (const auto& entry : parsed) {
        if (!bind_or_rollback(entry.first, action, entry.second)) {
            return 1;
        }
    }

    if (!cjsh_env::startup_active()) {
        std::vector<std::string> spec_strings;
        spec_strings.reserve(parsed.size());
        for (const auto& entry : parsed) {
            spec_strings.push_back(entry.second);
        }
        std::string action_display = canonical_action_name(action);
        std::cout << (replace_existing ? "Set " : "Added ") << action_display << " -> "
                  << join_specs(spec_strings) << '\n';
        std::cout << "Add `cjshopt keybind " << (replace_existing ? "set " : "add ")
                  << action_display << " '" << pipe_join_specs(spec_strings)
                  << "'` to your ~/.cjshrc to persist this change.\n";
        if (replace_existing && !keys_to_suppress.empty()) {
            std::vector<std::string> suppressed_specs;
            suppressed_specs.reserve(keys_to_suppress.size());
            for (const auto& entry : keys_to_suppress) {
                suppressed_specs.push_back(entry.second);
            }
            std::cout << "Disabled default bindings for " << action_display << ": "
                      << join_specs(suppressed_specs) << '\n';
        }
    }

    return 0;
}

int keybind_clear_keys_command(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                     "clear requires at least one key specification", keybind_usage_lines()});
        return 1;
    }

    std::vector<std::pair<ic_keycode_t, std::string>> parsed;
    if (!parse_key_specs_from_args(args, 2, parsed)) {
        return 1;
    }

    std::vector<std::string> removed;
    std::vector<std::string> missing;
    for (const auto& entry : parsed) {
        if (ic_clear_key_binding(entry.first)) {
            removed.push_back(entry.second);
        } else {
            missing.push_back(entry.second);
        }
    }

    if (!cjsh_env::startup_active()) {
        if (!removed.empty()) {
            std::cout << "Cleared key binding(s) for: " << join_specs(removed) << '\n';
        }
        if (!missing.empty()) {
            std::cout << "No custom binding found for: " << join_specs(missing) << '\n';
        }
        if (removed.empty() && missing.empty()) {
            std::cout << "Nothing to clear.\n";
        }
    }

    return 0;
}

int keybind_clear_action_command(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind", "clear-action requires an action name",
                     keybind_usage_lines()});
        return 1;
    }

    const std::string& action_arg = args[2];
    auto parsed_action = parse_key_action_or_report(action_arg);
    if (!parsed_action.has_value()) {
        return 1;
    }
    ic_key_action_t action = *parsed_action;

    auto entries = collect_bindings();
    std::vector<std::string> removed;
    for (const auto& entry : entries) {
        if (entry.action == action) {
            char buffer[64];
            if (ic_format_key_spec(entry.key, buffer, sizeof(buffer))) {
                (void)removed.emplace_back(buffer);
            }
            (void)ic_clear_key_binding(entry.key);
        }
    }

    std::vector<std::pair<ic_keycode_t, std::string>> default_keys;
    if (parse_default_action_keys(action, &default_keys)) {
        for (const auto& def : default_keys) {
            ic_key_action_t existing_action;
            if (ic_get_key_binding(def.first, &existing_action) &&
                existing_action == IC_KEY_ACTION_NONE) {
                (void)ic_clear_key_binding(def.first);
            }
        }
    }

    if (!cjsh_env::startup_active()) {
        if (!removed.empty()) {
            std::cout << "Cleared custom bindings for " << canonical_action_name(action) << ": "
                      << join_specs(removed) << '\n';
        } else {
            std::cout << "No custom bindings were set for " << canonical_action_name(action)
                      << ".\n";
        }
    }

    return 0;
}

int keybind_reset_command() {
    ic_reset_key_bindings();
    agent_mode::apply_key_bindings();
    browser::apply_key_bindings();
    if (!cjsh_env::startup_active()) {
        std::cout << "All custom key bindings cleared.\n";
    }
    return 0;
}
}  // namespace

int keybind_command(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind", "Missing subcommand argument",
                     keybind_usage_lines()});
        return 1;
    }

    const std::string& subcommand = args[1];
    if (subcommand == "--help" || subcommand == "-h") {
        print_keybind_usage();
        return 0;
    }

    if (subcommand == "ext") {
        return keybind_ext_command(args);
    }

    if (subcommand == "list") {
        if (args.size() != 2) {
            print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                         "list does not accept additional arguments", keybind_usage_lines()});
            return 1;
        }
        return keybind_list_command();
    }

    if (subcommand == "profile") {
        if (args.size() < 3) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "keybind",
                         "profile requires a subcommand",
                         {"Usage:", "  keybind profile list", "  keybind profile set <name>"}});
            return 1;
        }
        const std::string& profile_sub = args[2];
        if (profile_sub == "list") {
            if (args.size() != 3) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "keybind",
                             "profile list does not accept additional arguments",
                             {}});
                return 1;
            }
            return keybind_profile_list_command();
        }
        if (profile_sub == "set") {
            return keybind_profile_set_command(args);
        }
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind",
                     "Unknown profile subcommand '" + profile_sub + "'",
                     {"Valid profile subcommands are: list, set"}});
        return 1;
    }

    if (subcommand == "set") {
        return keybind_set_or_add_command(args, true);
    }

    if (subcommand == "add") {
        return keybind_set_or_add_command(args, false);
    }

    if (subcommand == "clear") {
        return keybind_clear_keys_command(args);
    }

    if (subcommand == "clear-action") {
        return keybind_clear_action_command(args);
    }

    if (subcommand == "reset") {
        if (args.size() != 2) {
            print_error({ErrorType::INVALID_ARGUMENT, "keybind",
                         "reset does not accept additional arguments", keybind_usage_lines()});
            return 1;
        }
        return keybind_reset_command();
    }

    print_error({ErrorType::INVALID_ARGUMENT, "keybind", "Unknown subcommand '" + subcommand + "'",
                 keybind_usage_lines()});
    return 1;
}
