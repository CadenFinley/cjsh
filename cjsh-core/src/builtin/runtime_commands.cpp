/*
  runtime_commands.cpp

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

#include "runtime_commands.h"
#include <string>
#include <string_view>
#include <vector>

#include "double_bracket_command.h"
#include "error_out.h"
#include "internal_subshell_command.h"
#include "shell_env.h"

namespace runtime_commands {

bool is_runtime_command_name(std::string_view command_name) {
    return command_name == "[[" || command_name == "__INTERNAL_SUBSHELL__" ||
           command_name == "__INTERNAL_BRACE_GROUP__";
}

int execute_runtime_command(const std::vector<std::string>& command_args, Shell* shell) {
    if (command_args.empty()) {
        return 1;
    }

    const std::string& command_name = command_args[0];
    if (command_name == "[[") {
        if (config::is_posix_mode()) {
            print_error({ErrorType::INVALID_ARGUMENT, "[[", "'[[' is disabled in POSIX mode", {}});
            return 2;
        }
        return double_bracket_command(command_args);
    }

    if (shell == nullptr) {
        return 1;
    }

    if (command_name == "__INTERNAL_SUBSHELL__") {
        return internal_subshell_command(command_args, shell);
    }
    if (command_name == "__INTERNAL_BRACE_GROUP__") {
        return internal_brace_group_command(command_args, shell);
    }

    return 1;
}

}  // namespace runtime_commands
