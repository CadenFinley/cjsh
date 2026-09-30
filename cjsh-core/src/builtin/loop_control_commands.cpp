/*
  loop_control_commands.cpp

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

#include "loop_control_commands.h"

#include "builtin.h"
#include "builtin_help.h"

#include <climits>
#include <string>
#include <vector>
#include "error_out.h"
#include "numeric_utils.h"
#include "shell_env.h"

namespace {

int set_loop_control_level(const std::vector<std::string>& args, const std::string& command,
                           const std::string& variable, int return_code) {
    int level = 1;
    if (args.size() > 1 && !numeric_utils::parse_int_in_range(args[1], 1, INT_MAX, level)) {
        print_error({ErrorType::INVALID_ARGUMENT, command, "invalid level: " + args[1], {}});
        return posix_special_builtin_error(1);
    }

    (void)cjsh_env::set_shell_variable_value(variable, std::to_string(level));
    return return_code;
}

}  // namespace

int break_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: break [N]", "Exit N levels of enclosing loops (default 1)."})) {
        return 0;
    }
    return set_loop_control_level(args, "break", "CJSH_BREAK_LEVEL", 255);
}

int continue_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: continue [N]",
                   "Skip to the next iteration of the current loop or Nth enclosing loop."})) {
        return 0;
    }
    return set_loop_control_level(args, "continue", "CJSH_CONTINUE_LEVEL", 254);
}

int return_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: return [N]",
                   "Exit a function with status N (default uses last command status)."})) {
        return 0;
    }
    int exit_code =
        numeric_utils::parse_exit_status_or(cjsh_env::get_shell_variable_value("?"), 0, false);
    if (args.size() > 1) {
        if (!numeric_utils::parse_int_in_range(args[1], 0, 255, exit_code)) {
            print_error(
                {ErrorType::INVALID_ARGUMENT, "return", "invalid exit code: " + args[1], {}});
            return posix_special_builtin_error(1);
        }
    }

    (void)cjsh_env::set_shell_variable_value("CJSH_RETURN_CODE", std::to_string(exit_code));

    return 253;
}
