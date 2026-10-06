/*
  shell_dialect.cpp

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

#include "shell_dialect.h"

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "shell_env.h"

namespace config {
namespace {
ShellDialect current_dialect = ShellDialect::Cjsh;
std::array<bool, 7> saved_native_settings{};
std::optional<std::string> saved_posixly_correct;
std::optional<std::string> saved_posixly_correct_variable;

std::array<bool*, 7> posix_settings() {
    return {&extglob_enabled,          &smart_cd_enabled, &script_extension_interpreter_enabled,
            &source_enabled,           &show_title_line,  &error_suggestions_enabled,
            &history_expansion_enabled};
}
}  // namespace

ShellDialect shell_dialect() {
    return current_dialect;
}
bool is_posix_mode() {
    return current_dialect == ShellDialect::Posix;
}

std::optional<ShellDialect> parse_shell_dialect(std::string_view name) {
    if (name == "cjsh") {
        return ShellDialect::Cjsh;
    }
    if (name == "posix") {
        return ShellDialect::Posix;
    }
    return std::nullopt;
}

const char* shell_dialect_name() {
    switch (current_dialect) {
        case ShellDialect::Cjsh:
            return "cjsh";
        case ShellDialect::Posix:
            return "posix";
    }
    return "cjsh";
}

void set_shell_dialect(ShellDialect dialect) {
    if (dialect == current_dialect) {
        return;
    }
    const auto settings = posix_settings();
    if (is_posix_mode()) {
        for (size_t i = 0; i < settings.size(); ++i) {
            *settings[i] = saved_native_settings[i];
        }
        // Startup imports POSIXLY_CORRECT into the shell's variable table. Keep
        // that table in sync when a running shell leaves the dialect, too.
        (void)cjsh_env::unset_shell_variable_value("POSIXLY_CORRECT");
        if (saved_posixly_correct_variable) {
            (void)cjsh_env::set_shell_variable_value("POSIXLY_CORRECT",
                                                     *saved_posixly_correct_variable);
        }
        if (saved_posixly_correct) {
            (void)setenv("POSIXLY_CORRECT", saved_posixly_correct->c_str(), 1);
        } else {
            (void)unsetenv("POSIXLY_CORRECT");
        }
    }
    if (dialect == ShellDialect::Posix) {
        for (size_t i = 0; i < settings.size(); ++i) {
            saved_native_settings[i] = *settings[i];
            *settings[i] = false;
        }
        const char* value = std::getenv("POSIXLY_CORRECT");
        saved_posixly_correct = value ? std::optional<std::string>(value) : std::nullopt;
        saved_posixly_correct_variable =
            cjsh_env::shell_variable_is_set("POSIXLY_CORRECT")
                ? std::optional<std::string>(cjsh_env::get_shell_variable_value("POSIXLY_CORRECT"))
                : std::nullopt;
        (void)setenv("POSIXLY_CORRECT", "1", 1);
        (void)cjsh_env::set_shell_variable_value("POSIXLY_CORRECT", "1");
    }
    current_dialect = dialect;
}

}  // namespace config
