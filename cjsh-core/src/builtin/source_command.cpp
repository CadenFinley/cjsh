/*
  source_command.cpp

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

#include "source_command.h"

#include "builtin_help.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>
#include "builtin.h"
#include "error_out.h"
#include "shell.h"
#include "shell_env.h"

int source_command(const std::vector<std::string>& args) {
    const bool invoked_as_source = !args.empty() && args[0] == "source";
    const std::string command_name = invoked_as_source ? "source" : ".";
    if (builtin_handle_help(args,
                            {"Usage: " + command_name + " FILE",
                             "Execute commands from FILE in the current shell environment."})) {
        return 0;
    }

    if (config::is_posix_mode() && invoked_as_source) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "source",
                     "'source' is disabled in POSIX mode. Use '.' instead.",
                     {}});
        return 1;
    }
    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, command_name, "missing file operand", {}});
        return posix_special_builtin_error(1);
    }

    if (!g_shell) {
        print_error({ErrorType::FATAL_ERROR, command_name, "shell not initialized properly", {}});
        return 1;
    }

    std::error_code status_ec;
    std::filesystem::path target_path(args[1]);
    if (config::is_posix_mode() && args[1].find('/') == std::string::npos) {
        const std::string path = cjsh_env::get_shell_variable_value("PATH");
        size_t start = 0;
        target_path.clear();
        while (start <= path.size()) {
            const auto end = path.find(':', start);
            auto candidate = std::filesystem::path(
                                 path.substr(start, end == std::string::npos ? end : end - start)) /
                             args[1];
            if (access(candidate.c_str(), R_OK) == 0 &&
                !std::filesystem::is_directory(candidate, status_ec)) {
                target_path = candidate;
                break;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (config::is_posix_mode() &&
        (target_path.empty() || access(target_path.c_str(), R_OK) != 0)) {
        print_error({ErrorType::FILE_NOT_FOUND, command_name, "cannot read file: " + args[1], {}});
        return posix_special_builtin_error(1);
    }
    if (std::filesystem::exists(target_path, status_ec) &&
        std::filesystem::is_directory(target_path, status_ec)) {
        print_error({ErrorType::RUNTIME_ERROR, command_name, "is a directory: " + args[1], {}});
        return posix_special_builtin_error(1);
    }

    return g_shell->execute_script_file(target_path);
}
