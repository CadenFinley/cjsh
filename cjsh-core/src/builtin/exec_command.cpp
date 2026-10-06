/*
  exec_command.cpp

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

#include "exec_command.h"
#include <sys/types.h>
#include <cstdint>

#include "builtin_help.h"

#include <fcntl.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "cjsh_filesystem.h"
#include "error_out.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"

namespace {

enum class FdOpOutcome : std::uint8_t {
    kNotHandled,
    kApplied,
    kError,
};

void print_exec_runtime_error(const std::string& detail) {
    print_error({ErrorType::RUNTIME_ERROR, "exec", detail, {}});
}

int exec_failure_exit_code(int exec_errno) {
    if (exec_errno == ENOENT) {
        return 127;
    }
    if (exec_errno == EACCES || exec_errno == ENOEXEC || exec_errno == EISDIR) {
        return 126;
    }
    return 1;
}

bool parse_fd_operation_token(const std::string& token, int& fd_num, std::string_view& op) {
    if (token.size() <= 1 || !std::isdigit(static_cast<unsigned char>(token[0]))) {
        return false;
    }

    size_t fd_end = 0;
    while (fd_end < token.size() && std::isdigit(static_cast<unsigned char>(token[fd_end]))) {
        ++fd_end;
    }

    if (fd_end == 0 || fd_end >= token.size()) {
        return false;
    }

    fd_num = std::stoi(token.substr(0, fd_end));
    op = std::string_view(token).substr(fd_end);
    return true;
}

FdOpOutcome apply_path_redirection(const std::vector<std::string>& args, size_t& index, int fd_num,
                                   std::string_view op) {
    if ((op != "<" && op != ">") || index + 1 >= args.size()) {
        return FdOpOutcome::kNotHandled;
    }

    int flags = (op == "<") ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    const std::string& filename = args[index + 1];
    auto redirect_result = cjsh_filesystem::redirect_fd(filename, fd_num, flags);
    if (redirect_result.is_error()) {
        print_exec_runtime_error(redirect_result.error());
        return FdOpOutcome::kError;
    }

    ++index;
    return FdOpOutcome::kApplied;
}

FdOpOutcome apply_fd_duplication(int fd_num, std::string_view op) {
    if ((op.rfind("<&", 0) != 0 && op.rfind(">&", 0) != 0) || op.size() <= 2) {
        return FdOpOutcome::kNotHandled;
    }

    std::string_view source_spec = op.substr(2);
    if (source_spec == "-") {
        cjsh_filesystem::safe_close(fd_num);
        return FdOpOutcome::kApplied;
    }

    try {
        int src_fd = std::stoi(std::string(source_spec));
        auto dup_result = cjsh_filesystem::safe_dup2(src_fd, fd_num);
        if (dup_result.is_error()) {
            print_exec_runtime_error(dup_result.error());
            return FdOpOutcome::kError;
        }
        return FdOpOutcome::kApplied;
    } catch (const std::exception&) {
        return FdOpOutcome::kNotHandled;
    }
}

FdOpOutcome try_apply_fd_operation(const std::vector<std::string>& args, size_t& index) {
    int fd_num = 0;
    std::string_view op;
    if (!parse_fd_operation_token(args[index], fd_num, op)) {
        return FdOpOutcome::kNotHandled;
    }

    FdOpOutcome path_redirection = apply_path_redirection(args, index, fd_num, op);
    if (path_redirection != FdOpOutcome::kNotHandled) {
        return path_redirection;
    }

    return apply_fd_duplication(fd_num, op);
}

int exec_replacing_shell(const std::vector<std::string>& exec_args) {
    auto c_args = cjsh_env::build_exec_argv(exec_args);
    int saved_errno;
    {
        cjsh_env::ReplacementShellLevel level;
        ExecSignalGuard signals;
        (void)execvp(exec_args[0].c_str(), c_args.data());
        saved_errno = errno;
    }
    print_exec_runtime_error(exec_args[0] + ": " + std::strerror(saved_errno));
    if (config::is_posix_mode() && !config::interactive_mode) {
        cjsh_env::request_exit();
        (void)cjsh_env::set_shell_variable_value(
            "EXIT_CODE", std::to_string(exec_failure_exit_code(saved_errno)));
    }
    return exec_failure_exit_code(saved_errno);
}

}  // namespace

int exec_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(
            args, {"Usage: exec [COMMAND [ARG ...]]", "Replace the current shell with COMMAND.",
                   "If COMMAND is omitted, apply redirections to the shell."})) {
        return 0;
    }
    (void)shell;

    if (args.size() <= 1) {
        return 0;
    }

    std::vector<std::string> exec_args;
    bool has_fd_operations = false;

    for (size_t i = 1; i < args.size(); ++i) {
        FdOpOutcome fd_outcome = try_apply_fd_operation(args, i);
        if (fd_outcome == FdOpOutcome::kError) {
            return 1;
        }
        if (fd_outcome == FdOpOutcome::kApplied) {
            has_fd_operations = true;
            continue;
        }

        exec_args.push_back(args[i]);
    }

    if (has_fd_operations && exec_args.empty()) {
        return 0;
    }

    if (!exec_args.empty()) {
        return exec_replacing_shell(exec_args);
    }

    return 0;
}
