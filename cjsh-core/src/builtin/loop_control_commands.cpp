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

#include <algorithm>
#include <climits>
#include <string>
#include <vector>
#include "control_flow.h"
#include "error_out.h"
#include "interpreter.h"
#include "numeric_utils.h"
#include "shell.h"
#include "shell_env.h"

namespace {

int set_loop_control_level(const std::vector<std::string>& args, const std::string& command,
                           ControlFlowKind kind) {
    int level = 1;
    if (args.size() > 1 && !numeric_utils::parse_int_in_range(args[1], 1, INT_MAX, level)) {
        print_error({ErrorType::INVALID_ARGUMENT, command, "invalid level: " + args[1], {}});
        return posix_special_builtin_error(1);
    }

    auto* interpreter = shell ? shell->get_interpreter() : nullptr;
    if (!interpreter || interpreter->enclosing_loop_count() == 0) {
        print_error({ErrorType::INVALID_ARGUMENT, command, command + " outside loop", {}});
        return 1;
    }
    interpreter->control_flow_state().request_loop(
        kind, std::min(level, interpreter->enclosing_loop_count()));
    return 0;
}

}  // namespace

int break_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: break [N]", "Exit N levels of enclosing loops (default 1)."})) {
        return 0;
    }
    return set_loop_control_level(args, "break", ControlFlowKind::Break);
}

int continue_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: continue [N]",
                   "Skip to the next iteration of the current loop or Nth enclosing loop."})) {
        return 0;
    }
    return set_loop_control_level(args, "continue", ControlFlowKind::Continue);
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

    auto* interpreter = shell ? shell->get_interpreter() : nullptr;
    if (!interpreter || (!interpreter->in_function_scope() && !interpreter->in_source_scope())) {
        print_error({ErrorType::INVALID_ARGUMENT, "return", "return outside function", {}});
        return 1;
    }
    interpreter->control_flow_state().request_return(exit_code);
    return exit_code;
}
