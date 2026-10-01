/*
  command_lookup.h

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

#ifndef CJSH_CORE_SRC_UTILS_COMMAND_LOOKUP_H
#define CJSH_CORE_SRC_UTILS_COMMAND_LOOKUP_H

#include <cstdint>
#include <string>
#include <vector>
class Shell;

namespace command_lookup {

struct CommandResolution {
    bool is_keyword = false;
    bool is_builtin = false;
    bool has_alias = false;
    std::string alias_value;
    bool has_function = false;
    bool has_path = false;
    std::string path;
};

enum class CommandResolutionKind : std::uint8_t {
    Keyword,
    Builtin,
    Alias,
    Function,
    Path
};

struct CommandResolutionEntry {
    CommandResolutionKind kind;
    std::string value;
};

bool is_shell_keyword(const std::string& token);
bool is_shell_control_structure_leader(const std::string& token);
const std::vector<std::string>& shell_control_structure_keywords();
bool is_shell_builtin(const std::string& token, Shell* shell);
bool token_allows_split_command_merge(const std::string& token);
bool lookup_shell_alias(const std::string& token, Shell* shell, std::string& alias_value);
bool has_shell_function(const std::string& token, Shell* shell);
bool should_auto_cd_token(const std::string& token, Shell* shell, bool* directory_result = nullptr);
CommandResolution resolve_command(const std::string& token, Shell* shell, bool include_path = true);
std::vector<CommandResolutionEntry> list_resolution_entries(const std::string& token, Shell* shell,
                                                            bool include_path = true);

}  // namespace command_lookup

#endif  // CJSH_CORE_SRC_UTILS_COMMAND_LOOKUP_H
