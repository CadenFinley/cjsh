/*
  cjshopt_keybind_ext_command.cpp

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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agent_mode.h"
#include "browser.h"
#include "error_out.h"
#include "isocline.h"
#include "keybindings.h"
#include "shell_env.h"

namespace {
std::uint64_t g_bindings_revision = 0;
std::unordered_map<ic_keycode_t, custom_command_binding_t> g_custom_keybindings;
std::unordered_map<std::string, custom_command_binding_t> g_custom_palette_commands;

template <typename Map>
std::string binding_field_or_empty(const Map& bindings, const typename Map::key_type& key,
                                   std::string custom_command_binding_t::* field) {
    auto it = bindings.find(key);
    return it == bindings.end() ? std::string{} : it->second.*field;
}

template <typename Map>
auto sorted_binding_entries(const Map& bindings)
    -> std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> {
    std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> entries;
    entries.reserve(bindings.size());
    for (const auto& entry : bindings) {
        entries.emplace_back(entry.first, entry.second);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    return entries;
}
}  // namespace

std::uint64_t custom_command_bindings_revision() {
    return g_bindings_revision;
}

std::string get_custom_keybinding(ic_keycode_t key) {
    return binding_field_or_empty(g_custom_keybindings, key, &custom_command_binding_t::command);
}

bool has_custom_keybinding(ic_keycode_t key) {
    return g_custom_keybindings.find(key) != g_custom_keybindings.end();
}

void set_custom_keybinding(ic_keycode_t key, const std::string& command, const std::string& title) {
    g_custom_keybindings[key] = custom_command_binding_t{title, command};
    ++g_bindings_revision;
}

void clear_custom_keybinding(ic_keycode_t key) {
    (void)g_custom_keybindings.erase(key);
    ++g_bindings_revision;
}

void clear_all_custom_keybindings() {
    g_custom_keybindings.clear();
    ++g_bindings_revision;
}

std::vector<std::pair<ic_keycode_t, custom_command_binding_t>> list_custom_keybindings() {
    return sorted_binding_entries(g_custom_keybindings);
}

std::string get_custom_palette_command(const std::string& id) {
    return binding_field_or_empty(g_custom_palette_commands, id,
                                  &custom_command_binding_t::command);
}

bool has_custom_palette_command(const std::string& id) {
    return g_custom_palette_commands.find(id) != g_custom_palette_commands.end();
}

void set_custom_palette_command(const std::string& id, const std::string& command,
                                const std::string& title) {
    g_custom_palette_commands[id] = custom_command_binding_t{title, command};
    ++g_bindings_revision;
}

void clear_custom_palette_command(const std::string& id) {
    (void)g_custom_palette_commands.erase(id);
    ++g_bindings_revision;
}

void clear_all_custom_palette_commands() {
    g_custom_palette_commands.clear();
    ++g_bindings_revision;
}

std::vector<std::pair<std::string, custom_command_binding_t>> list_custom_palette_commands() {
    return sorted_binding_entries(g_custom_palette_commands);
}

namespace {
constexpr const char* kPalettePrefix = "palette:";

bool parse_palette_spec(const std::string& key_spec, std::string* out_palette_id) {
    if (out_palette_id == nullptr) {
        return false;
    }
    if (key_spec.rfind(kPalettePrefix, 0) != 0) {
        return false;
    }

    std::string id = key_spec.substr(std::strlen(kPalettePrefix));
    if (id.empty()) {
        return false;
    }
    *out_palette_id = id;
    return true;
}

std::optional<std::string> parse_palette_spec_or_report(const std::string& key_spec) {
    std::string palette_id;
    if (!parse_palette_spec(key_spec, &palette_id)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind ext",
                     "Invalid palette identifier '" + key_spec + "'",
                     {"Palette-only entries must use format 'palette:<id>'"}});
        return std::nullopt;
    }
    return palette_id;
}

const std::vector<std::string>& keybind_ext_usage_lines() {
    static const std::vector<std::string> kUsage = {
        "Usage: keybind ext <subcommand> [...]",
        "",
        "Subcommands:",
        "  list                   Show custom key and palette command bindings",
        "  set <key> [--title <title>] <cmd> Bind a key to execute a command",
        "  set palette:<id> [--title <title>] <cmd> Add palette-only command",
        "  clear <key|palette:id>... Remove custom command bindings",
        "  reset                  Clear all custom command bindings",
        "",
        "Examples:",
        "  keybind ext set 'ctrl-g' 'echo Hello!'",
        "  keybind ext set 'ctrl-g' --title 'Accept line' 'cjsh-widget accept'",
        "  keybind ext set 'palette:uuid' 'uuidgen'",
        "  keybind ext set 'palette:uuid' --title 'Generate UUID' 'uuidgen'",
        "  keybind ext set 'F5' 'clear'",
        "  keybind ext list",
        "  keybind ext clear 'ctrl-g' 'palette:uuid'",
        "  keybind ext reset",
    };
    return kUsage;
}

void print_keybind_ext_usage() {
    if (cjsh_env::startup_active()) {
        return;
    }
    for (const auto& line : keybind_ext_usage_lines()) {
        std::cout << line << '\n';
    }
}

int keybind_ext_list_command() {
    if (cjsh_env::startup_active()) {
        return 0;
    }

    auto sorted_bindings = list_custom_keybindings();
    auto sorted_palette = list_custom_palette_commands();

    if (sorted_bindings.empty() && sorted_palette.empty()) {
        std::cout << "No custom command bindings are currently defined.\n";
        std::cout << "Add 'cjshopt keybind ext set <key> [--title <title>] <command>' for "
                     "keybound actions or 'cjshopt keybind ext set palette:<id> [--title "
                     "<title>] <command>' for palette-only snippets in ~/.cjshrc.\n";
        return 0;
    }

    if (!sorted_bindings.empty()) {
        std::cout << "Custom command keybindings:\n\n";
        std::cout << std::left << std::setw(20) << "Key" << std::setw(28) << "Title"
                  << "Command\n";
        std::cout << std::string(96, '-') << '\n';
        for (const auto& [key, binding] : sorted_bindings) {
            char buffer[64];
            if (ic_format_key_spec(key, buffer, sizeof(buffer))) {
                std::string title = binding.title.empty() ? binding.command : binding.title;
                std::cout << std::left << std::setw(20) << buffer << std::setw(28) << title
                          << binding.command << '\n';
            }
        }
        std::cout << '\n';
    }

    if (!sorted_palette.empty()) {
        std::cout << "Palette-only commands:\n\n";
        std::cout << std::left << std::setw(24) << "Palette ID" << std::setw(28) << "Title"
                  << "Command\n";
        std::cout << std::string(104, '-') << '\n';
        for (const auto& [id, binding] : sorted_palette) {
            std::string title = binding.title.empty() ? id : binding.title;
            std::cout << std::left << std::setw(24) << id << std::setw(28) << title
                      << binding.command << '\n';
        }
        std::cout << '\n';
    }

    std::cout << "These bindings are defined in your configuration files.\n";
    std::cout << "To modify them, edit your ~/.cjshrc file.\n";

    return 0;
}

int keybind_ext_set_command(const std::vector<std::string>& args) {
    if (args.size() < 5) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind ext",
                     "set requires a key specification and a command",
                     {"Usage: keybind ext set <key_spec|palette:id> [--title <title>] <command>",
                      "Example: keybind ext set 'ctrl-g' 'echo Hello from ctrl-g!'",
                      "Example: keybind ext set 'ctrl-g' --title 'Accept line' "
                      "'cjsh-widget accept'",
                      "Example: keybind ext set 'palette:uuid' --title 'Generate UUID' "
                      "'uuidgen'"}});
        return 1;
    }

    const std::string& key_spec = args[3];
    std::string title;
    size_t command_start = 4;

    if (args[4] == "--title") {
        if (args.size() < 7) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "keybind ext",
                         "--title requires a title and a command",
                         {"Usage: keybind ext set <key_spec|palette:id> --title <title> "
                          "<command>",
                          "Example: keybind ext set 'ctrl-g' --title 'Accept line' "
                          "'cjsh-widget accept'"}});
            return 1;
        }
        title = args[5];
        command_start = 6;
    } else if (args[4].rfind("--title=", 0) == 0) {
        title = args[4].substr(std::strlen("--title="));
        if (title.empty()) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "keybind ext",
                         "--title option cannot be empty",
                         {"Usage: keybind ext set <key_spec|palette:id> --title <title> "
                          "<command>"}});
            return 1;
        }
        command_start = 5;
    }

    if (command_start >= args.size()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind ext",
                     "set requires a command",
                     {"Usage: keybind ext set <key_spec|palette:id> [--title <title>] <command>"}});
        return 1;
    }

    std::string command;
    for (size_t i = command_start; i < args.size(); ++i) {
        if (i > command_start) {
            command += ' ';
        }
        command += args[i];
    }

    if (key_spec.rfind(kPalettePrefix, 0) == 0) {
        auto palette_id = parse_palette_spec_or_report(key_spec);
        if (!palette_id.has_value()) {
            return 1;
        }

        std::string effective_title = title.empty() ? *palette_id : title;
        set_custom_palette_command(*palette_id, command, effective_title);
        if (!cjsh_env::startup_active()) {
            std::cout << "Registered palette-only command '" << *palette_id << "' (title: '"
                      << effective_title << "'): " << command << '\n';
            std::cout << "Add `cjshopt keybind ext set 'palette:" << *palette_id << "'";
            if (!title.empty()) {
                std::cout << " --title '" << title << "'";
            }
            std::cout << " '" << command << "'` to your ~/.cjshrc to persist this change.\n";
        }
        return 0;
    }

    ic_keycode_t key_code = 0;
    if (!ic_parse_key_spec(key_spec.c_str(), &key_code)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind ext",
                     "Invalid key specification '" + key_spec + "'",
                     {"Use key specs like 'ctrl-g', 'alt-h', 'F5', or palette IDs like "
                      "'palette:uuid'."}});
        return 1;
    }

    ic_key_action_t existing_action;
    if (ic_get_key_binding(key_code, &existing_action) &&
        (existing_action != IC_KEY_ACTION_RUNOFF)) {
        if (!cjsh_env::startup_active()) {
            const char* action_name = ic_key_action_name(existing_action);
            std::string bound_name = action_name ? action_name : "(unknown action)";
            print_error({ErrorType::INVALID_ARGUMENT,
                         ErrorSeverity::WARNING,
                         "keybind ext",
                         "Key '" + key_spec + "' is already bound to '" + bound_name +
                             "' and will be overridden.",
                         {"Use 'cjshopt keybind ext list' to review custom bindings."}});
        }

        (void)ic_clear_key_binding(key_code);
    }

    if (!ic_bind_key(key_code, IC_KEY_ACTION_RUNOFF)) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "keybind ext",
                     "Failed to bind key specification '" + key_spec + "'",
                     {}});
        return 1;
    }

    std::string effective_title = title.empty() ? command : title;
    set_custom_keybinding(key_code, command, effective_title);

    if (!cjsh_env::startup_active()) {
        std::cout << "Bound key '" << key_spec << "' (title: '" << effective_title
                  << "') to command: " << command << '\n';
        std::cout << "Add `cjshopt keybind ext set '" << key_spec << "'";
        if (!title.empty()) {
            std::cout << " --title '" << title << "'";
        }
        std::cout << " '" << command << "'` to your ~/.cjshrc to persist this change.\n";
    }

    return 0;
}

int keybind_ext_clear_command(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "keybind ext",
                     "clear requires at least one key specification or palette id",
                     {"Usage: keybind ext clear <key_spec|palette:id> [<...>]",
                      "Example: keybind ext clear 'ctrl-g' 'palette:uuid'"}});
        return 1;
    }

    std::vector<std::string> cleared;
    std::vector<std::string> not_found;

    for (size_t i = 3; i < args.size(); ++i) {
        const std::string& key_spec = args[i];

        if (key_spec.rfind(kPalettePrefix, 0) == 0) {
            auto palette_id = parse_palette_spec_or_report(key_spec);
            if (!palette_id.has_value()) {
                continue;
            }

            if (has_custom_palette_command(*palette_id)) {
                clear_custom_palette_command(*palette_id);
                cleared.push_back(key_spec);
            } else {
                not_found.push_back(key_spec);
            }
            continue;
        }

        ic_keycode_t key_code = 0;
        if (!ic_parse_key_spec(key_spec.c_str(), &key_code)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "keybind ext",
                         "Invalid key specification '" + key_spec + "'",
                         {}});
            continue;
        }

        if (has_custom_keybinding(key_code)) {
            clear_custom_keybinding(key_code);
            (void)ic_clear_key_binding(key_code);
            cleared.push_back(key_spec);
        } else {
            not_found.push_back(key_spec);
        }
    }

    if (!cjsh_env::startup_active()) {
        if (!cleared.empty()) {
            std::cout << "Cleared custom command binding(s) for: ";
            for (size_t i = 0; i < cleared.size(); ++i) {
                if (i > 0) {
                    std::cout << ", ";
                }
                std::cout << cleared[i];
            }
            std::cout << '\n';
        }
        if (!not_found.empty()) {
            std::cout << "No custom command binding found for: ";
            for (size_t i = 0; i < not_found.size(); ++i) {
                if (i > 0) {
                    std::cout << ", ";
                }
                std::cout << not_found[i];
            }
            std::cout << '\n';
        }
    }

    agent_mode::apply_key_bindings();
    browser::apply_key_bindings();

    return cleared.empty() ? 1 : 0;
}

int keybind_ext_reset_command() {
    for (const auto& [key, _] : g_custom_keybindings) {
        (void)ic_clear_key_binding(key);
    }
    clear_all_custom_keybindings();
    clear_all_custom_palette_commands();
    agent_mode::apply_key_bindings();
    browser::apply_key_bindings();

    if (!cjsh_env::startup_active()) {
        std::cout << "All custom command keybindings and palette-only commands cleared.\n";
    }

    return 0;
}
}  // namespace

int keybind_ext_command(const std::vector<std::string>& args) {
    if (args.size() >= 3 && (args[2] == "--help" || args[2] == "-h")) {
        print_keybind_ext_usage();
        return 0;
    }

    if (args.size() < 3) {
        print_error({ErrorType::INVALID_ARGUMENT, "keybind ext", "Missing subcommand",
                     keybind_ext_usage_lines()});
        return 1;
    }

    const std::string& subcommand = args[2];

    if (subcommand == "list") {
        return keybind_ext_list_command();
    }

    if (subcommand == "set") {
        return keybind_ext_set_command(args);
    }

    if (subcommand == "clear") {
        return keybind_ext_clear_command(args);
    }

    if (subcommand == "reset") {
        return keybind_ext_reset_command();
    }

    print_error({ErrorType::INVALID_ARGUMENT,
                 "keybind ext",
                 "Unknown subcommand '" + subcommand + "'",
                 {"Available subcommands: list, set, clear, reset"}});
    return 1;
}
