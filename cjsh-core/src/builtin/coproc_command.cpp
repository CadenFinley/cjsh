/*
  coproc_command.cpp

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

#include "coproc_command.h"
#include <algorithm>
#include <iterator>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "builtin_help.h"
#include "error_out.h"
#include "interpreter.h"
#include "job_control.h"
#include "parser_utils.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"

namespace {

int coprocess_read_fd = -1;
int coprocess_write_fd = -1;

void close_fd(int& descriptor) {
    if (descriptor >= 0) {
        (void)close(descriptor);
        descriptor = -1;
    }
}

void close_pipe_pair(int descriptors[2]) {
    close_fd(descriptors[0]);
    close_fd(descriptors[1]);
}

void set_close_on_exec(int descriptor) {
    int flags = fcntl(descriptor, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
}

std::string display_command(const std::vector<std::string>& command) {
    std::string result = "coproc";
    for (const auto& argument : command) {
        result.push_back(' ');
        result += argument;
    }
    return result;
}

std::string trim_copy(std::string value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

bool starts_with_word(std::string_view text, std::string_view word) {
    return text == word || (text.size() > word.size() && text.substr(0, word.size()) == word &&
                            (std::isspace(static_cast<unsigned char>(text[word.size()])) != 0 ||
                             text[word.size()] == ';'));
}

bool starts_compound_command(const std::string& text) {
    std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed.front() == '{' || trimmed.front() == '(' || trimmed.rfind("[[", 0) == 0 ||
        trimmed.rfind("((", 0) == 0) {
        return true;
    }
    static constexpr std::string_view compound_words[] = {"if",    "for",  "select",  "while",
                                                          "until", "case", "function"};
    return std::any_of(std::begin(compound_words), std::end(compound_words),
                       [&](std::string_view word) { return starts_with_word(trimmed, word); });
}

int launch_coprocess(const std::string& variable_name, const std::string& command_display,
                     Shell* shell, const std::function<int()>& run_child) {
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
        int saved_errno = errno;
        close_pipe_pair(input_pipe);
        close_pipe_pair(output_pipe);
        print_error({ErrorType::RUNTIME_ERROR,
                     "coproc",
                     "failed to create pipes: " + std::string(std::strerror(saved_errno)),
                     {}});
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        int saved_errno = errno;
        close_pipe_pair(input_pipe);
        close_pipe_pair(output_pipe);
        print_error({ErrorType::RUNTIME_ERROR,
                     "coproc",
                     "failed to fork: " + std::string(std::strerror(saved_errno)),
                     {}});
        return 1;
    }

    if (child == 0) {
        (void)setpgid(0, 0);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGQUIT, SIG_DFL);
        (void)signal(SIGTSTP, SIG_DFL);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        if (dup2(input_pipe[0], STDIN_FILENO) < 0 || dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(1);
        }
        close_fd(input_pipe[0]);
        close_fd(output_pipe[1]);
        int status = run_child();
        std::cout.flush();
        std::cerr.flush();
        _exit(status & 0xff);
    }

    (void)setpgid(child, child);
    close_fd(input_pipe[0]);
    close_fd(output_pipe[1]);
    close_fd(coprocess_read_fd);
    close_fd(coprocess_write_fd);
    coprocess_read_fd = output_pipe[0];
    coprocess_write_fd = input_pipe[1];
    set_close_on_exec(coprocess_read_fd);
    set_close_on_exec(coprocess_write_fd);

    auto& variables = shell->get_interpreter()->get_variable_manager();
    (void)variables.assign_global_array_literal(
        variable_name, {std::to_string(coprocess_read_fd), std::to_string(coprocess_write_fd)},
        false);
    (void)variables.assign_global_variable(variable_name + "_PID", std::to_string(child), false);

    auto& jobs = JobManager::instance();
    int job_id = jobs.add_job(child, {child}, command_display, true, false);
    if (auto job = jobs.get_job(job_id)) {
        job->suppress_notifications = true;
    }
    jobs.set_last_background_pid(child);
    return 0;
}

}  // namespace

int coproc_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(
            args, {"Usage: coproc COMMAND [ARG ...]", "       coproc [NAME] COMPOUND_COMMAND",
                   "Run a command asynchronously with a two-way pipe.",
                   "Descriptors are NAME[0]/NAME[1] and the process ID is NAME_PID.",
                   "The default NAME is COPROC."})) {
        return 0;
    }
    if (config::is_posix_mode()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "coproc",
                     "'coproc' is not available in POSIX mode",
                     {"Run without --posix to use coprocesses"}});
        return 1;
    }
    if (shell == nullptr || shell->get_interpreter() == nullptr) {
        print_error({ErrorType::RUNTIME_ERROR, "coproc", "shell interpreter not available", {}});
        return 1;
    }
    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "coproc", "missing coprocess command", {}});
        return 2;
    }

    std::vector<std::string> command(args.begin() + 1, args.end());
    return launch_coprocess("COPROC", display_command(command), shell,
                            [shell, command] { return shell->execute_command(command); });
}

int coproc_script_command(const std::string& command_text, Shell* shell) {
    if (config::is_posix_mode()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "coproc",
                     "'coproc' is not available in POSIX mode",
                     {"Run without --posix to use coprocesses"}});
        return 1;
    }
    if (shell == nullptr || shell->get_interpreter() == nullptr) {
        print_error({ErrorType::RUNTIME_ERROR, "coproc", "shell interpreter not available", {}});
        return 1;
    }

    std::string remainder = trim_copy(command_text.substr(std::string_view("coproc").size()));
    if (remainder.empty()) {
        print_error({ErrorType::INVALID_ARGUMENT, "coproc", "missing coprocess command", {}});
        return 2;
    }
    if (remainder == "--help") {
        return coproc_command({"coproc", remainder}, shell);
    }

    std::string variable_name = "COPROC";
    std::string script = remainder;
    if (!starts_compound_command(remainder)) {
        size_t word_end = remainder.find_first_of(" \t\r\n");
        if (word_end != std::string::npos) {
            std::string candidate = remainder.substr(0, word_end);
            std::string after_candidate = trim_copy(remainder.substr(word_end));
            if (is_valid_identifier(candidate) && starts_compound_command(after_candidate)) {
                variable_name = candidate;
                script = std::move(after_candidate);
            }
        }
    }

    return launch_coprocess(variable_name, "coproc " + remainder, shell, [shell, script] {
        return shell->get_interpreter()->execute_block({script});
    });
}
