/*
  command_lookup.cpp

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

#include "command_lookup.h"
#include <string>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "interpreter.h"
#include "shell.h"
#include "token_constants.h"

namespace command_lookup {

const std::vector<std::string>& shell_control_structure_keywords() {
    return token_constants::shell_control_structure_keywords();
}

bool is_shell_control_structure_leader(const std::string& token) {
    return token_constants::shell_control_structure_leaders().find(token) !=
           token_constants::shell_control_structure_leaders().end();
}

bool is_shell_keyword(const std::string& token) {
    if (token_constants::shell_keywords().count(token) > 0) {
        return true;
    }

    return token == "{" || token == "}" || token == "(" || token == ")" || token == "[[" ||
           token == "]]" || token == "!";
}

bool is_shell_builtin(const std::string& token, Shell* shell) {
    return shell != nullptr && shell->get_built_ins() != nullptr &&
           shell->get_built_ins()->is_builtin_command(token) != 0;
}

bool token_allows_split_command_merge(const std::string& token) {
    if (token.empty() || token[0] == '-' || token.find('=') != std::string::npos) {
        return false;
    }

    static const std::string kDisallowedChars = "/\\'\"`$|&;(){}[]<>";
    return token.find_first_of(kDisallowedChars) == std::string::npos;
}

bool lookup_shell_alias(const std::string& token, Shell* shell, std::string& alias_value) {
    if (shell == nullptr) {
        return false;
    }

    const auto& aliases = shell->get_aliases();
    auto alias_it = aliases.find(token);
    if (alias_it == aliases.end()) {
        return false;
    }

    alias_value = alias_it->second;
    return true;
}

bool has_shell_function(const std::string& token, Shell* shell) {
    if (shell == nullptr) {
        return false;
    }

    const auto* interpreter = shell->get_shell_script_interpreter();
    return interpreter != nullptr && interpreter->has_function(token);
}

bool should_auto_cd_token(const std::string& token, Shell* shell, bool* directory_result) {
    if (directory_result != nullptr) {
        *directory_result = false;
    }
    if (shell == nullptr || token.empty() || !shell->get_shell_option(ShellOption::Autocd)) {
        return false;
    }

    // Treat "./" as explicit execution.
    if (token.rfind("./", 0) == 0) {
        return false;
    }

    Built_ins* built_ins = shell->get_built_ins();
    if (built_ins == nullptr) {
        return false;
    }

    const std::string cwd = built_ins->get_current_directory();
    const std::string previous_directory = shell->get_previous_directory();
    const bool is_directory =
        cjsh_filesystem::is_auto_cd_directory_token(token, cwd, previous_directory);
    if (directory_result != nullptr) {
        *directory_result = is_directory;
    }
    if (!is_directory) {
        return false;
    }

    CommandResolution resolution = resolve_command(token, shell, false);
    if (resolution.has_alias || resolution.is_builtin || resolution.has_function) {
        return false;
    }

    return !cjsh_filesystem::resolves_to_executable(token, cwd);
}

CommandResolution resolve_command(const std::string& token, Shell* shell, bool include_path) {
    CommandResolution resolution;
    resolution.is_keyword = is_shell_keyword(token);
    resolution.is_builtin = is_shell_builtin(token, shell);
    resolution.has_alias = lookup_shell_alias(token, shell, resolution.alias_value);
    resolution.has_function = has_shell_function(token, shell);

    if (include_path) {
        resolution.path = cjsh_filesystem::find_executable_in_path(token);
        resolution.has_path = !resolution.path.empty();
    }

    return resolution;
}

std::vector<CommandResolutionEntry> list_resolution_entries(const std::string& token, Shell* shell,
                                                            bool include_path) {
    std::vector<CommandResolutionEntry> entries;
    const auto resolution = resolve_command(token, shell, include_path);

    if (resolution.is_keyword) {
        entries.push_back({CommandResolutionKind::Keyword, {}});
    }
    if (resolution.is_builtin) {
        entries.push_back({CommandResolutionKind::Builtin, {}});
    }
    if (resolution.has_alias) {
        entries.push_back({CommandResolutionKind::Alias, resolution.alias_value});
    }
    if (resolution.has_function) {
        entries.push_back({CommandResolutionKind::Function, {}});
    }
    if (resolution.has_path) {
        entries.push_back({CommandResolutionKind::Path, resolution.path});
    }

    return entries;
}

}  // namespace command_lookup
