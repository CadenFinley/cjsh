/*
  exec.cpp

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

#include "exec.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "error_out.h"
#include "interpreter.h"
#include "job_control.h"
#include "parser.h"
#include "readonly_command.h"
#include "script_dispatch.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "suggestion_utils.h"
#include "wait_status_utils.h"

namespace {

int extract_exit_code(int status) {
    return wait_status_utils::to_exit_code(status, 1);
}

void flush_standard_streams_before_fork() {
    (void)std::cout.flush();
    (void)std::cerr.flush();
    (void)std::clog.flush();
    (void)std::fflush(nullptr);
}

pid_t fork_command_child() {
    // A signal can arrive before fork finishes restoring the child's runtime.
    // Keep it pending until reset_child_signals installs the command's defaults;
    // inherited handlers (including raise on macOS) are not safe in that window.
    sigset_t blocked_signals{};
    sigset_t previous_mask{};
    sigfillset(&blocked_signals);
    if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_mask) < 0) {
        return -1;
    }

    const pid_t pid = fork();
    const int fork_errno = errno;
    if (pid != 0) {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    }
    errno = fork_errno;
    return pid;
}

int set_process_group(pid_t pid, pid_t pgid) {
    if (setpgid(pid, pgid) == 0) {
        return 0;
    }

    const int saved_errno = errno;
    // Parent and child both establish the job's group. On macOS their concurrent
    // calls can report EPERM even though the requested group is already in place.
    const pid_t target_pgid = pgid == 0 ? (pid == 0 ? getpid() : pid) : pgid;
    if (saved_errno == EPERM && getpgid(pid) == target_pgid) {
        return 0;
    }
    errno = saved_errno;
    return -1;
}

std::string join_arguments(const std::vector<std::string>& args) {
    return string_utils::join_strings(args, " ");
}

[[noreturn]] void exec_external_child(const std::vector<std::string>& args,
                                      const char* cached_path);

struct PtyPair {
    int master_fd{-1};
    int slave_fd{-1};
};

std::optional<PtyPair> create_output_pty(int terminal_fd) {
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        return std::nullopt;
    }
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        cjsh_filesystem::safe_close(master_fd);
        return std::nullopt;
    }
    char* slave_name = ptsname(master_fd);
    if (slave_name == nullptr) {
        cjsh_filesystem::safe_close(master_fd);
        return std::nullopt;
    }

    int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        cjsh_filesystem::safe_close(master_fd);
        return std::nullopt;
    }

    struct termios term_state{};
    if (tcgetattr(terminal_fd, &term_state) == 0) {
        (void)tcsetattr(slave_fd, TCSANOW, &term_state);
    }

    struct winsize ws{};
    if (ioctl(terminal_fd, TIOCGWINSZ, &ws) == 0) {
        (void)ioctl(slave_fd, TIOCSWINSZ, &ws);
    }

    (void)cjsh_filesystem::set_close_on_exec(master_fd);

    return PtyPair{master_fd, slave_fd};
}

std::shared_ptr<OutputRelayState> start_output_relay(int master_fd, bool forward) {
    auto relay = std::make_shared<OutputRelayState>();
    relay->master_fd = master_fd;
    relay->forward.store(forward);

    try {
        std::thread t([relay] {
            char buffer[4096];
            while (true) {
                ssize_t bytes_read = read(relay->master_fd, buffer, sizeof(buffer));
                if (bytes_read == 0) {
                    break;
                }
                if (bytes_read < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (relay->forward.load()) {
                    (void)cjsh_filesystem::write_all(
                        STDOUT_FILENO, std::string_view(buffer, static_cast<size_t>(bytes_read)));
                }
            }
            cjsh_filesystem::safe_close(relay->master_fd);
        });
        t.detach();
    } catch (...) {
        cjsh_filesystem::safe_close(relay->master_fd);
        throw;
    }

    return relay;
}

struct CommandExecutionPlan {
    bool is_builtin{false};
    std::string cached_exec_path;
};

int execute_builtin_or_special_command(const std::vector<std::string>& cmd_args) {
    Built_ins* built_ins = g_shell ? g_shell->get_built_ins() : nullptr;
    if (built_ins != nullptr) {
        return built_ins->builtin_or_runtime_command(cmd_args);
    }

    return 1;
}

bool is_builtin_or_special_command(const std::vector<std::string>& cmd_args) {
    if (cmd_args.empty() || !g_shell) {
        return false;
    }

    Built_ins* built_ins = g_shell->get_built_ins();
    return built_ins != nullptr && (built_ins->is_builtin_or_runtime_command(cmd_args[0]) != 0);
}

CommandExecutionPlan resolve_command_exec_plan(const std::vector<std::string>& cmd_args,
                                               std::optional<bool> known_builtin = std::nullopt) {
    CommandExecutionPlan plan;
    plan.is_builtin = known_builtin ? *known_builtin : is_builtin_or_special_command(cmd_args);

    if (!cmd_args.empty() && !plan.is_builtin) {
        plan.cached_exec_path = cjsh_filesystem::resolve_executable_for_execution(cmd_args[0]);
    }

    return plan;
}

[[noreturn]] void exec_builtin_or_external_child(const std::vector<std::string>& cmd_args,
                                                 bool is_builtin,
                                                 const std::string& cached_exec_path) {
    if (is_builtin) {
        int exit_code = execute_builtin_or_special_command(cmd_args);
        (void)fflush(stdout);
        (void)fflush(stderr);
        _exit(exit_code);
    }

    const char* exec_override = cached_exec_path.empty() ? nullptr : cached_exec_path.c_str();
    exec_external_child(cmd_args, exec_override);
}

void attach_output_relay_to_job(Job& job, const std::optional<PtyPair>& output_pty,
                                std::shared_ptr<OutputRelayState>& output_relay) {
    if (!output_pty.has_value()) {
        return;
    }

    cjsh_filesystem::safe_close(output_pty->slave_fd);
    output_relay = start_output_relay(output_pty->master_fd, !job.background);
    job.output_relay = output_relay;
}

bool command_has_stdout_redirection(const Command& cmd) {
    return !cmd.output_file.empty() || !cmd.append_file.empty() || cmd.both_output ||
           cmd.stdout_to_stderr || cmd.has_fd_redirection(STDOUT_FILENO) ||
           cmd.has_fd_duplication(STDOUT_FILENO);
}

bool command_has_stderr_redirection(const Command& cmd) {
    return !cmd.stderr_file.empty() || cmd.stderr_to_stdout || cmd.both_output ||
           cmd.has_fd_redirection(STDERR_FILENO) || cmd.has_fd_duplication(STDERR_FILENO);
}

void apply_assignments_to_shell_env(
    const std::vector<std::pair<std::string, std::string>>& assignments) {
    if (!g_shell || assignments.empty()) {
        return;
    }

    auto& env_vars = cjsh_env::env_vars();
    for (const auto& env : assignments) {
        env_vars[env.first] = env.second;
        cjsh_env::mirror_set_to_process_env(env.first, env.second);
        cjsh_env::sync_parser_env_var(g_shell.get(), env.first);
    }
}

Job make_single_process_job(pid_t pid, const std::string& command, bool background,
                            bool auto_background_on_stop, bool auto_background_on_stop_silent,
                            bool process_group = true) {
    Job job;
    job.pgid = pid;
    job.command = command;
    job.background = background;
    job.auto_background_on_stop = auto_background_on_stop;
    job.auto_background_on_stop_silent = auto_background_on_stop_silent;
    job.process_group = process_group;
    job.completed = false;
    job.stopped = false;
    job.pids.push_back(pid);
    job.last_pid = pid;
    job.pid_order.push_back(pid);
    job.pipeline_statuses.assign(1, -1);
    return job;
}

bool is_shell_control_structure(const Command& cmd) {
    if (cmd.args.empty()) {
        return false;
    }

    return command_lookup::is_shell_control_structure_leader(cmd.args[0]);
}

std::string command_text_for_interpretation(const Command& cmd) {
    if (!cmd.original_text.empty()) {
        return cmd.original_text;
    }
    return join_arguments(cmd.args);
}

struct ProcessSubstitutionResources {
    std::string directory_path;
    std::vector<std::string> fifo_paths;
    std::vector<pid_t> child_pids;
};

void cleanup_process_substitutions(ProcessSubstitutionResources& resources,
                                   bool terminate_children = false);

std::string create_process_substitution_directory() {
    const char* configured_temp_dir = std::getenv("TMPDIR");
    std::string temp_dir = configured_temp_dir != nullptr && configured_temp_dir[0] != '\0'
                               ? configured_temp_dir
                               : "/tmp";
    while (temp_dir.size() > 1 && temp_dir.back() == '/') {
        temp_dir.pop_back();
    }

    std::string directory_template = temp_dir + "/cjsh_procsub_XXXXXX";
    std::vector<char> directory_buffer(directory_template.begin(), directory_template.end());
    directory_buffer.push_back('\0');

    char* created_directory = mkdtemp(directory_buffer.data());
    if (created_directory == nullptr) {
        throw std::runtime_error("cjsh: failed to create private process substitution directory: " +
                                 std::string(strerror(errno)));
    }

    if (chmod(created_directory, S_IRWXU) == -1) {
        int saved_errno = errno;
        (void)rmdir(created_directory);
        throw std::runtime_error("cjsh: failed to secure process substitution directory: " +
                                 std::string(strerror(saved_errno)));
    }

    return created_directory;
}

void replace_all_instances(std::string& target, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = target.find(from, pos)) != std::string::npos) {
        (void)target.replace(pos, from.length(), to);
        pos += to.length();
    }
}

ErrorType classify_filesystem_error(int err) {
    switch (err) {
        case ENOENT:
            return ErrorType::FILE_NOT_FOUND;
        case EACCES:
            return ErrorType::PERMISSION_DENIED;
        default:
            return ErrorType::RUNTIME_ERROR;
    }
}

[[noreturn]] void child_exit_with_error(ErrorType type, const std::string& command,
                                        const std::string& message) {
    ErrorInfo info{type, ErrorSeverity::ERROR, command, message, {}};
    print_error(info);
    _exit(EXIT_FAILURE);
}

bool replace_first_instance(std::string& target, const std::string& from, const std::string& to) {
    size_t pos = target.find(from);
    if (pos == std::string::npos) {
        return false;
    }
    (void)target.replace(pos, from.length(), to);
    return true;
}

std::atomic<int> g_command_not_found_handler_depth{0};

bool special_handlers_enabled() {
    return !config::minimal_mode && !config::secure_mode && !config::is_posix_mode();
}

std::vector<std::string> build_command_not_found_suggestions(const std::string& command_name) {
    return suggestion_utils::generate_command_suggestions_if_enabled(command_name);
}

bool handler_defers_to_default_command_not_found_output(
    const std::optional<int>& handler_exit_code) {
    return handler_exit_code.has_value() &&
           handler_exit_code.value() == ShellScriptInterpreter::exit_command_not_found;
}

std::optional<int> maybe_invoke_command_not_found_handler(const std::vector<std::string>& args) {
    if (!special_handlers_enabled() || args.empty() || !g_shell) {
        return std::nullopt;
    }

    ShellScriptInterpreter* interpreter = g_shell->get_shell_script_interpreter();
    if (interpreter == nullptr || !interpreter->has_function("command_not_found_handler")) {
        return std::nullopt;
    }

    static thread_local bool handler_active = false;
    if (handler_active) {
        return std::nullopt;
    }

    std::vector<std::string> handler_args;
    handler_args.reserve(args.size() + 1);
    (void)handler_args.emplace_back("command_not_found_handler");
    (void)handler_args.insert(handler_args.end(), args.begin(), args.end());

    handler_active = true;
    (void)g_command_not_found_handler_depth.fetch_add(1, std::memory_order_relaxed);
    int handler_exit_code = ShellScriptInterpreter::exit_command_not_found;
    try {
        handler_exit_code = interpreter->invoke_function(handler_args);
    } catch (...) {
        (void)g_command_not_found_handler_depth.fetch_sub(1, std::memory_order_relaxed);
        handler_active = false;
        return std::nullopt;
    }

    (void)g_command_not_found_handler_depth.fetch_sub(1, std::memory_order_relaxed);
    handler_active = false;
    return handler_exit_code;
}

bool should_try_command_not_found_handler(const std::vector<std::string>& args, bool is_builtin,
                                          const std::string& cached_exec_path) {
    if (!special_handlers_enabled() || args.empty() || is_builtin || !cached_exec_path.empty()) {
        return false;
    }

    return args[0].find('/') == std::string::npos;
}

[[noreturn]] void report_exec_failure(const std::vector<std::string>& args, int saved_errno) {
    const std::string command_name = args.empty() ? std::string{} : args[0];

    if (saved_errno == ENOENT) {
        const bool has_explicit_path = command_name.find('/') != std::string::npos;
        if (has_explicit_path) {
            const char* err_detail = strerror(saved_errno);
            std::string message_detail = err_detail ? err_detail : "no such file or directory";
            print_error({ErrorType::FILE_NOT_FOUND,
                         ErrorSeverity::ERROR,
                         command_name,
                         message_detail,
                         {"try checking the file path or creating the file."}});
            _exit(ShellScriptInterpreter::exit_command_not_found);
        }

        std::vector<std::string> suggestions;
        suggestions = build_command_not_found_suggestions(command_name);

        auto handler_exit_code = maybe_invoke_command_not_found_handler(args);
        if (handler_exit_code.has_value() &&
            !handler_defers_to_default_command_not_found_output(handler_exit_code)) {
            _exit(handler_exit_code.value());
        }

        print_error(
            {ErrorType::COMMAND_NOT_FOUND, ErrorSeverity::ERROR, command_name, "", suggestions});
        _exit(ShellScriptInterpreter::exit_command_not_found);
    }

    const bool permission_error = (saved_errno == EACCES || saved_errno == EISDIR);
    const bool exec_format_error = (saved_errno == ENOEXEC);
    const int exit_code = (permission_error || exec_format_error) ? 126 : 127;

    std::string detail = permission_error    ? "permission denied"
                         : exec_format_error ? "exec format error"
                                             : "execution failed";

    std::string message_detail;
    if (exec_format_error) {
        message_detail = "exec format error";
    } else if (!permission_error) {
        message_detail = detail;
        if (saved_errno != 0) {
            message_detail += ": ";
            message_detail += strerror(saved_errno);
        }
    }

    ErrorType error_type = permission_error    ? ErrorType::PERMISSION_DENIED
                           : exec_format_error ? ErrorType::UNKNOWN_ERROR
                                               : ErrorType::RUNTIME_ERROR;

    std::vector<std::string> suggestions;
    if (config::error_suggestions_enabled && !message_detail.empty()) {
        suggestions.push_back("Detail: " + message_detail);
    }

    print_error({error_type, ErrorSeverity::ERROR, command_name, message_detail, suggestions});
    _exit(exit_code);
}

bool strip_temporary_env_assignments(
    std::vector<std::string>& args, size_t cmd_start_idx, size_t original_arg_count,
    const std::vector<std::pair<std::string, std::string>>& assignments) {
    const bool has_temporary_env = !assignments.empty() && cmd_start_idx < original_arg_count;
    if (has_temporary_env && cmd_start_idx > 0) {
        auto erase_end =
            args.begin() + static_cast<std::vector<std::string>::difference_type>(cmd_start_idx);
        (void)args.erase(args.begin(), erase_end);
    }
    return has_temporary_env;
}

using cjsh_env::TemporaryEnvAssignmentScope;

ProcessSubstitutionResources setup_process_substitutions(Command& cmd) {
    ProcessSubstitutionResources resources;

    if (!g_shell || cmd.process_substitutions.empty()) {
        return resources;
    }

    try {
        resources.directory_path = create_process_substitution_directory();
        resources.fifo_paths.reserve(cmd.process_substitutions.size());
        resources.child_pids.reserve(cmd.process_substitutions.size());

        for (size_t i = 0; i < cmd.process_substitutions.size(); ++i) {
            const std::string& proc_sub = cmd.process_substitutions[i];

            if (proc_sub.length() < 4 || proc_sub.back() != ')' ||
                (proc_sub[0] != '<' && proc_sub[0] != '>') || proc_sub[1] != '(') {
                throw std::runtime_error("cjsh: invalid process substitution: " + proc_sub);
            }

            bool is_input = proc_sub[0] == '<';
            std::string command = proc_sub.substr(2, proc_sub.length() - 3);

            std::string fifo_path = resources.directory_path + "/fifo_" + std::to_string(i);
            if (mkfifo(fifo_path.c_str(), 0600) == -1) {
                throw std::runtime_error("cjsh: failed to create FIFO for process substitution '" +
                                         proc_sub + "': " + std::string(strerror(errno)));
            }

            pid_t pid = fork();
            if (pid == -1) {
                (void)unlink(fifo_path.c_str());
                throw std::runtime_error("cjsh: failed to fork for process substitution '" +
                                         proc_sub + "': " + std::string(strerror(errno)));
            }

            if (pid == 0) {
                const std::string substitution_label =
                    command.empty() ? "process substitution" : command;
                auto exit_with_error = [&](ErrorType type, const std::string& message) {
                    child_exit_with_error(type, substitution_label, message);
                };

                if (is_input) {
                    auto fifo_result = cjsh_filesystem::safe_open(fifo_path, O_WRONLY);
                    if (fifo_result.is_error()) {
                        exit_with_error(
                            ErrorType::FILE_NOT_FOUND,
                            "open: failed to open FIFO for writing: " + fifo_result.error());
                    }

                    auto dup_result =
                        cjsh_filesystem::safe_dup2(fifo_result.value(), STDOUT_FILENO);
                    if (dup_result.is_error()) {
                        exit_with_error(
                            ErrorType::RUNTIME_ERROR,
                            "dup2: failed to duplicate stdout descriptor: " + dup_result.error());
                    }

                    cjsh_filesystem::safe_close(fifo_result.value());
                } else {
                    auto fifo_result = cjsh_filesystem::safe_open(fifo_path, O_RDONLY);
                    if (fifo_result.is_error()) {
                        exit_with_error(
                            ErrorType::FILE_NOT_FOUND,
                            "open: failed to open FIFO for reading: " + fifo_result.error());
                    }

                    auto dup_result = cjsh_filesystem::safe_dup2(fifo_result.value(), STDIN_FILENO);
                    if (dup_result.is_error()) {
                        cjsh_filesystem::safe_close(fifo_result.value());
                        exit_with_error(
                            ErrorType::RUNTIME_ERROR,
                            "dup2: failed to duplicate stdin descriptor: " + dup_result.error());
                    }

                    cjsh_filesystem::safe_close(fifo_result.value());
                }

                int result = g_shell->execute(command);
                _exit(result);
            }

            resources.child_pids.push_back(pid);
            resources.fifo_paths.push_back(fifo_path);

            bool replaced_arg = false;
            for (auto& arg : cmd.args) {
                if (replace_first_instance(arg, proc_sub, fifo_path)) {
                    replaced_arg = true;
                    break;
                }
            }

            if (!cmd.input_file.empty()) {
                (void)replace_first_instance(cmd.input_file, proc_sub, fifo_path);
            }
            if (!cmd.output_file.empty()) {
                (void)replace_first_instance(cmd.output_file, proc_sub, fifo_path);
            }
            if (!cmd.append_file.empty()) {
                (void)replace_first_instance(cmd.append_file, proc_sub, fifo_path);
            }
            if (!cmd.stderr_file.empty()) {
                (void)replace_first_instance(cmd.stderr_file, proc_sub, fifo_path);
            }
            if (!cmd.both_output_file.empty()) {
                (void)replace_first_instance(cmd.both_output_file, proc_sub, fifo_path);
            }
            for (auto& [fd, path] : cmd.fd_redirections) {
                (void)replace_first_instance(path, proc_sub, fifo_path);
            }
            for (auto& redirection : cmd.redirection_order) {
                (void)replace_first_instance(redirection.value, proc_sub, fifo_path);
            }

            if (!replaced_arg) {
                for (auto& arg : cmd.args) {
                    replace_all_instances(arg, proc_sub, fifo_path);
                }
            }
        }
    } catch (...) {
        cleanup_process_substitutions(resources, true);
        throw;
    }

    return resources;
}

void cleanup_process_substitutions(ProcessSubstitutionResources& resources,
                                   bool terminate_children) {
    if (terminate_children) {
        for (pid_t pid : resources.child_pids) {
            if (pid > 0) {
                (void)kill(pid, SIGTERM);
            }
        }
    }

    for (pid_t pid : resources.child_pids) {
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    }

    for (const std::string& path : resources.fifo_paths) {
        (void)unlink(path.c_str());
    }
    if (!resources.directory_path.empty()) {
        (void)rmdir(resources.directory_path.c_str());
    }

    resources.child_pids.clear();
    resources.fifo_paths.clear();
    resources.directory_path.clear();
}

enum class FdOperationErrorType : std::uint8_t {
    Redirect,
    Duplication
};

struct FdOperationError {
    FdOperationErrorType type;
    int fd_num;
    int src_fd;
    std::string spec;
    std::string error;
};

struct RedirectSpecInfo {
    std::string file;
    int flags{0};
};

RedirectSpecInfo parse_fd_redirect_spec(int fd_num, const std::string& spec) {
    RedirectSpecInfo info;

    if (spec.rfind("input:", 0) == 0) {
        info.file = spec.substr(6);
        info.flags = O_RDONLY;
    } else if (spec.rfind("output:", 0) == 0) {
        info.file = spec.substr(7);
        info.flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else {
        info.file = spec;
        info.flags = (fd_num == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
    }

    return info;
}

template <typename FailureHandler>
bool apply_fd_operations(const Command& cmd, FailureHandler&& on_failure) {
    for (const auto& fd_redir : cmd.fd_redirections) {
        int fd_num = fd_redir.first;
        const std::string& spec = fd_redir.second;
        RedirectSpecInfo info = parse_fd_redirect_spec(fd_num, spec);

        if (((info.flags & O_WRONLY) != 0 && (info.flags & O_TRUNC) != 0) &&
            cjsh_filesystem::should_noclobber_prevent_overwrite(info.file)) {
            on_failure(FdOperationError{FdOperationErrorType::Redirect, fd_num, -1, spec,
                                        "cannot overwrite existing file (noclobber is set)"});
            return false;
        }

        auto redirect_result = cjsh_filesystem::redirect_fd(info.file, fd_num, info.flags);
        if (redirect_result.is_error()) {
            on_failure(FdOperationError{FdOperationErrorType::Redirect, fd_num, -1, spec,
                                        redirect_result.error()});
            return false;
        }
    }

    for (const auto& fd_dup : cmd.fd_duplications) {
        int dst_fd = fd_dup.first;
        int src_fd = fd_dup.second;

        if (src_fd == -1) {
            cjsh_filesystem::safe_close(dst_fd);
            continue;
        }

        auto dup_result = cjsh_filesystem::safe_dup2(src_fd, dst_fd);
        if (dup_result.is_error()) {
            on_failure(FdOperationError{FdOperationErrorType::Duplication, dst_fd, src_fd, "",
                                        dup_result.error()});
            return false;
        }
    }

    return true;
}

enum class HereDocErrorKind : std::uint8_t {
    Pipe,
    ContentWrite,
    NewlineWrite,
    Duplication
};

enum class HereDocErrorStyle : std::uint8_t {
    Standard,
    ChildProcess
};

std::string format_here_document_error(HereDocErrorKind kind, const std::string& detail,
                                       HereDocErrorStyle style = HereDocErrorStyle::Standard) {
    const bool child_process = style == HereDocErrorStyle::ChildProcess;
    switch (kind) {
        case HereDocErrorKind::Pipe:
            return (child_process ? "pipe: failed to secure here document pipe: "
                                  : "failed to create pipe for here document: ") +
                   detail;
        case HereDocErrorKind::ContentWrite:
            return (child_process ? "write: failed to write here document content: "
                                  : "failed to write here document content: ") +
                   detail;
        case HereDocErrorKind::NewlineWrite:
            return (child_process ? "write: failed to write here document newline: "
                                  : "failed to write here document newline: ") +
                   detail;
        case HereDocErrorKind::Duplication:
            return (child_process ? "dup2: failed to duplicate here document descriptor: "
                                  : "failed to duplicate here document descriptor: ") +
                   detail;
    }
    return detail;
}

template <typename ErrorHandler>
bool setup_here_document_stdin(const std::string& here_doc, ErrorHandler&& on_error);

template <typename ErrorHandler>
bool apply_ordered_redirections(const Command& cmd, ErrorHandler&& on_error) {
    auto fail = [&](ErrorType type, const std::string& message) {
        on_error(type, message);
        return false;
    };

    auto redirect_file = [&](const std::string& path, int fd, int flags, bool force_overwrite,
                             const std::string& label) {
        if ((flags & O_WRONLY) != 0 && (flags & O_TRUNC) != 0 &&
            cjsh_filesystem::should_noclobber_prevent_overwrite(path, force_overwrite)) {
            return fail(ErrorType::PERMISSION_DENIED,
                        path + ": cannot overwrite existing file (noclobber is set)");
        }

        auto redirect_result = cjsh_filesystem::redirect_fd(path, fd, flags);
        if (redirect_result.is_error()) {
            return fail(classify_filesystem_error(errno),
                        label + path + ": " + redirect_result.error());
        }
        return true;
    };

    for (const auto& redirection : cmd.redirection_order) {
        switch (redirection.type) {
            case CommandRedirectionType::Input:
                if (!redirect_file(redirection.value, STDIN_FILENO, O_RDONLY, false, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::Output:
                if (!redirect_file(redirection.value, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC,
                                   false, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::Append:
                if (!redirect_file(redirection.value, STDOUT_FILENO, O_WRONLY | O_CREAT | O_APPEND,
                                   true, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::ForceOutput:
                if (!redirect_file(redirection.value, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC,
                                   true, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::StderrOutput:
                if (!redirect_file(redirection.value, STDERR_FILENO, O_WRONLY | O_CREAT | O_TRUNC,
                                   false, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::StderrAppend:
                if (!redirect_file(redirection.value, STDERR_FILENO, O_WRONLY | O_CREAT | O_APPEND,
                                   true, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::Duplicate: {
                auto dup_result = cjsh_filesystem::safe_dup2(redirection.target_fd, redirection.fd);
                if (dup_result.is_error()) {
                    return fail(ErrorType::RUNTIME_ERROR,
                                "dup2 failed for " + std::to_string(redirection.fd) + ">&" +
                                    std::to_string(redirection.target_fd) + ": " +
                                    dup_result.error());
                }
                break;
            }
            case CommandRedirectionType::Close:
                cjsh_filesystem::safe_close(redirection.fd);
                break;
            case CommandRedirectionType::BothOutput: {
                if (!redirect_file(redirection.value, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC,
                                   false, "")) {
                    return false;
                }
                auto dup_result = cjsh_filesystem::safe_dup2(STDOUT_FILENO, STDERR_FILENO);
                if (dup_result.is_error()) {
                    return fail(ErrorType::RUNTIME_ERROR,
                                "dup2 failed for stderr in &> redirection: " + dup_result.error());
                }
                break;
            }
            case CommandRedirectionType::HereDoc: {
                auto here_doc_error = [&](HereDocErrorKind kind, const std::string& detail) {
                    on_error(ErrorType::RUNTIME_ERROR, format_here_document_error(kind, detail));
                };

                if (!setup_here_document_stdin(redirection.value, here_doc_error)) {
                    return false;
                }
                break;
            }
            case CommandRedirectionType::HereString: {
                auto here_error = cjsh_filesystem::setup_here_string_stdin(redirection.value);
                if (here_error.has_value()) {
                    std::string message;
                    switch (here_error->type) {
                        case cjsh_filesystem::HereStringErrorType::Pipe:
                            message =
                                "failed to create pipe for here string: " + here_error->detail;
                            break;
                        case cjsh_filesystem::HereStringErrorType::Write:
                            message = "failed to write here string content: " + here_error->detail;
                            break;
                        case cjsh_filesystem::HereStringErrorType::Dup:
                            message =
                                "failed to duplicate here string descriptor: " + here_error->detail;
                            break;
                    }
                    return fail(ErrorType::RUNTIME_ERROR, message);
                }
                break;
            }
            case CommandRedirectionType::FdInput:
                if (!redirect_file(redirection.value, redirection.fd, O_RDONLY, false, "")) {
                    return false;
                }
                break;
            case CommandRedirectionType::FdOutput:
                if (!redirect_file(redirection.value, redirection.fd, O_WRONLY | O_CREAT | O_TRUNC,
                                   false, "")) {
                    return false;
                }
                break;
        }
    }

    return true;
}

template <typename ErrorHandler>
bool setup_here_document_stdin(const std::string& here_doc, ErrorHandler&& on_error) {
    int here_pipe[2] = {-1, -1};
    auto pipe_result = cjsh_filesystem::create_pipe_cloexec(here_pipe);
    if (pipe_result.is_error()) {
        on_error(HereDocErrorKind::Pipe, pipe_result.error());
        return false;
    }

    const auto duplicate_pipe_to_stdin = [&]() -> bool {
        auto dup_result = cjsh_filesystem::duplicate_pipe_read_end_to_fd(here_pipe, STDIN_FILENO);
        if (dup_result.is_error()) {
            on_error(HereDocErrorKind::Duplication, dup_result.error());
            return false;
        }
        return true;
    };

    auto write_result = cjsh_filesystem::write_all(here_pipe[1], std::string_view{here_doc});
    if (write_result.is_error()) {
        if (!cjsh_filesystem::error_indicates_broken_pipe(write_result.error())) {
            on_error(HereDocErrorKind::ContentWrite, write_result.error());
            cjsh_filesystem::close_pipe(here_pipe);
            return false;
        }

        if (!duplicate_pipe_to_stdin()) {
            return false;
        }
        return true;
    }

    if (!duplicate_pipe_to_stdin()) {
        return false;
    }

    return true;
}

enum class StreamRedirectErrorKind : std::uint8_t {
    Noclobber,
    Redirect,
    Duplication
};

struct StreamRedirectError {
    StreamRedirectErrorKind kind;
    std::string target;
    std::string detail;
    int src_fd;
    int dst_fd;
};

[[noreturn]] void handle_stream_redirect_error_and_exit(const StreamRedirectError& error) {
    ErrorInfo info;
    info.severity = ErrorSeverity::ERROR;

    switch (error.kind) {
        case StreamRedirectErrorKind::Noclobber:
            info.type = ErrorType::PERMISSION_DENIED;
            info.command_used = error.target.empty() ? "redirect" : error.target;
            info.message = error.detail;
            break;
        case StreamRedirectErrorKind::Redirect:
            info.type = ErrorType::RUNTIME_ERROR;
            info.command_used = error.target.empty() ? "redirect" : error.target;
            info.message = error.detail;
            break;
        case StreamRedirectErrorKind::Duplication: {
            info.type = ErrorType::RUNTIME_ERROR;
            info.command_used = "dup2";
            std::ostringstream oss;
            if (error.src_fd == STDOUT_FILENO && error.dst_fd == STDERR_FILENO) {
                oss << "2>&1 failed: " << error.detail;
            } else {
                oss << error.dst_fd << ">&" << error.src_fd << " failed: " << error.detail;
            }
            info.message = oss.str();
            break;
        }
    }

    print_error(info);
    _exit(EXIT_FAILURE);
}

[[noreturn]] void handle_fd_operation_error_and_exit(const FdOperationError& error) {
    ErrorInfo info;
    info.severity = ErrorSeverity::ERROR;
    info.command_used = error.type == FdOperationErrorType::Redirect ? error.spec : "dup2";

    if (error.type == FdOperationErrorType::Redirect) {
        info.type = ErrorType::FILE_NOT_FOUND;
        info.message = error.error;
    } else {
        info.type = ErrorType::RUNTIME_ERROR;
        info.message = "failed for " + std::to_string(error.fd_num) + ">&" +
                       std::to_string(error.src_fd) + ": " + error.error;
    }

    print_error(info);
    _exit(EXIT_FAILURE);
}

template <typename ErrorHandler>
bool configure_stderr_redirects(const Command& cmd, ErrorHandler&& on_error) {
    if (!cmd.stderr_file.empty()) {
        if (!cmd.stderr_append &&
            cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.stderr_file)) {
            on_error(StreamRedirectError{StreamRedirectErrorKind::Noclobber, cmd.stderr_file,
                                         "cannot overwrite existing file (noclobber is set)", -1,
                                         -1});
            return false;
        }

        int flags = O_WRONLY | O_CREAT | (cmd.stderr_append ? O_APPEND : O_TRUNC);
        auto redirect_result = cjsh_filesystem::redirect_fd(cmd.stderr_file, STDERR_FILENO, flags);
        if (redirect_result.is_error()) {
            on_error(StreamRedirectError{StreamRedirectErrorKind::Redirect, cmd.stderr_file,
                                         redirect_result.error(), -1, STDERR_FILENO});
            return false;
        }
    } else if (cmd.stderr_to_stdout) {
        auto dup_result = cjsh_filesystem::safe_dup2(STDOUT_FILENO, STDERR_FILENO);
        if (dup_result.is_error()) {
            on_error(StreamRedirectError{StreamRedirectErrorKind::Duplication, "",
                                         dup_result.error(), STDOUT_FILENO, STDERR_FILENO});
            return false;
        }
    }

    if (cmd.stdout_to_stderr) {
        auto dup_result = cjsh_filesystem::safe_dup2(STDERR_FILENO, STDOUT_FILENO);
        if (dup_result.is_error()) {
            on_error(StreamRedirectError{StreamRedirectErrorKind::Duplication, "",
                                         dup_result.error(), STDERR_FILENO, STDOUT_FILENO});
            return false;
        }
    }

    return true;
}

[[noreturn]] void exec_external_child(const std::vector<std::string>& args,
                                      const char* cached_path) {
    if (config::script_extension_interpreter_enabled && !config::is_posix_mode()) {
        auto interpreter_args =
            script_dispatch::build_extension_interpreter_args(args, cached_path);
        if (interpreter_args) {
            auto c_interp_args = cjsh_env::build_exec_argv(*interpreter_args);
            (void)execvp((*interpreter_args)[0].c_str(), c_interp_args.data());
            int saved_errno = errno;
            report_exec_failure(*interpreter_args, saved_errno);
        }
    }
    auto c_args = cjsh_env::build_exec_argv(args);
    if (cached_path != nullptr && cached_path[0] != '\0') {
        (void)execv(cached_path, c_args.data());
        int saved_errno = errno;
        report_exec_failure(args, saved_errno);
    }
    (void)execvp(args[0].c_str(), c_args.data());
    int saved_errno = errno;
    report_exec_failure(args, saved_errno);
}

}  // namespace

Exec::Exec()
    : shell_pgid(getpid()),
      shell_terminal(STDIN_FILENO),
      shell_is_interactive(false),
      last_pipeline_statuses(1, 0) {
    bool requested_interactive = config::interactive_mode || config::force_interactive;
    shell_is_interactive = requested_interactive;

#ifdef O_CLOEXEC
    int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
#else
    int tty_fd = open("/dev/tty", O_RDWR);
#endif
    if (tty_fd >= 0) {
        shell_terminal = tty_fd;
        owns_shell_terminal = true;
    }
}

Exec::~Exec() {
    int status = 0;
    int zombie_count = 0;
    const int max_cleanup_iterations = 50;
    while (zombie_count < max_cleanup_iterations) {
        pid_t reap_pid = waitpid(-1, &status, WNOHANG);
        if (reap_pid <= 0) {
            break;
        }
        zombie_count++;
    }

    if (zombie_count >= max_cleanup_iterations) {
        print_error({ErrorType::RUNTIME_ERROR,
                     ErrorSeverity::WARNING,
                     "exec",
                     "destructor hit maximum cleanup iterations, some zombies may remain",
                     {}});
    }

    if (owns_shell_terminal && shell_terminal >= 0) {
        (void)close(shell_terminal);
        shell_terminal = STDIN_FILENO;
        owns_shell_terminal = false;
    }
}

bool Exec::handle_empty_args(const std::vector<std::string>& args) {
    if (!args.empty()) {
        return false;
    }

    set_error(ErrorType::INVALID_ARGUMENT, "",
              "cannot execute empty command - no arguments provided");
    last_exit_code = EX_DATAERR;
    return true;
}

std::optional<int> Exec::handle_prepared_assignments(const cjsh_env::PreparedCommand& command,
                                                     bool asynchronous) {
    if (handle_empty_args(command.original_args)) {
        set_last_pipeline_statuses({last_exit_code});
        return last_exit_code;
    }
    if (!command.args.empty()) {
        return std::nullopt;
    }
    if (asynchronous) {
        cjsh_env::apply_env_assignments(command.assignments);
        set_error(ErrorType::RUNTIME_ERROR, "", "Environment variables set", {});
    } else {
        apply_assignments_to_shell_env(command.assignments);
    }
    last_exit_code = 0;
    set_last_pipeline_statuses({0});
    return 0;
}

std::optional<int> Exec::run_command_not_found_handler(
    const std::vector<std::string>& args,
    const std::vector<std::pair<std::string, std::string>>& assignments, bool is_builtin,
    const std::string& cached_exec_path) {
    if (!should_try_command_not_found_handler(args, is_builtin, cached_exec_path)) {
        return std::nullopt;
    }

    TemporaryEnvAssignmentScope temp_scope(g_shell.get(), assignments);
    const auto handler_exit_code = maybe_invoke_command_not_found_handler(args);
    if (!handler_exit_code.has_value()) {
        return std::nullopt;
    }

    const std::string command_name = args.empty() ? std::string{} : args[0];
    std::vector<std::string> suggestions = build_command_not_found_suggestions(command_name);
    const bool use_default_output =
        handler_defers_to_default_command_not_found_output(handler_exit_code);
    if (use_default_output) {
        print_error(
            {ErrorType::COMMAND_NOT_FOUND, ErrorSeverity::ERROR, command_name, "", suggestions});
    }
    set_error(ErrorType::COMMAND_NOT_FOUND, command_name, "", suggestions);
    return use_default_output ? ShellScriptInterpreter::exit_command_not_found
                              : handler_exit_code.value();
}

bool Exec::requires_fork(const Command& cmd) const {
    return !cmd.input_file.empty() || !cmd.output_file.empty() || !cmd.append_file.empty() ||
           cmd.background || !cmd.stderr_file.empty() || cmd.stderr_to_stdout ||
           cmd.stdout_to_stderr || !cmd.here_doc.empty() || !cmd.here_string.empty() ||
           cmd.both_output || !cmd.process_substitutions.empty() || !cmd.fd_redirections.empty() ||
           !cmd.fd_duplications.empty();
}

bool Exec::can_execute_in_process(const Command& cmd) const {
    if (cmd.args.empty()) {
        return false;
    }

    if (is_builtin_or_special_command(cmd.args)) {
        return !requires_fork(cmd);
    }

    return false;
}

int Exec::execute_builtin_with_redirections(Command cmd) {
    if (!g_shell || (g_shell->get_built_ins() == nullptr)) {
        set_error(ErrorType::FATAL_ERROR, "builtin",
                  "no shell context available for builtin execution");
        last_exit_code = EX_SOFTWARE;
        return EX_SOFTWARE;
    }

    bool persist_fd_changes = (!cmd.args.empty() && cmd.args[0] == "exec" && cmd.args.size() == 1);
    std::string command_name = cmd.args.empty() ? "builtin" : cmd.args[0];

    auto action = [&]() -> int { return execute_builtin_or_special_command(cmd.args); };

    bool action_invoked = false;
    int exit_code = run_with_command_redirections(cmd, action, command_name, persist_fd_changes,
                                                  &action_invoked);

    if (!action_invoked) {
        last_exit_code = exit_code;
        if (is_posix_special_builtin(command_name)) {
            (void)cjsh_env::posix_error_exit(exit_code);
        }
        return exit_code;
    }

    auto exit_result = job_utils::make_exit_error_result(command_name, exit_code,
                                                         "builtin command completed successfully",
                                                         "builtin command failed with exit code ");
    set_error(exit_result.type, command_name, exit_result.message, exit_result.suggestions);
    last_exit_code = exit_code;
    return exit_code;
}

void Exec::warn_parent_setpgid_failure() {
    if (errno != EACCES && errno != ESRCH) {
        set_error(ErrorType::RUNTIME_ERROR, "setpgid",
                  "failed to set process group ID in parent: " + std::string(strerror(errno)));
    }
}

void Exec::set_last_pipeline_statuses(std::vector<int> statuses) {
    last_pipeline_statuses = std::move(statuses);
}

int Exec::execute_command_sync(const std::vector<std::string>& args, bool auto_background_on_stop,
                               bool auto_background_on_stop_silent) {
    return execute_prepared_command_sync(cjsh_env::prepare_command(args), auto_background_on_stop,
                                         auto_background_on_stop_silent);
}

int Exec::execute_prepared_command_sync(cjsh_env::PreparedCommand command,
                                        bool auto_background_on_stop,
                                        bool auto_background_on_stop_silent) {
    const auto& args = command.original_args;
    if (g_shell) {
        g_shell->mark_terminal_dirty();
    }
    const bool monitor_mode = g_shell && g_shell->is_job_control_enabled();
    if (auto status = handle_prepared_assignments(command, false)) {
        return *status;
    }
    const auto& env_assignments = command.assignments;
    auto& cmd_args_value = command.args;

    Command proc_cmd;
    proc_cmd.args = cmd_args_value;
    for (const auto& arg : cmd_args_value) {
        if (arg.size() >= 4 && arg.back() == ')' &&
            (arg.rfind("<(", 0) == 0 || arg.rfind(">(", 0) == 0)) {
            proc_cmd.process_substitutions.push_back(arg);
        }
    }

    ProcessSubstitutionResources proc_resources;
    if (!proc_cmd.process_substitutions.empty()) {
        try {
            proc_resources = setup_process_substitutions(proc_cmd);
            cmd_args_value = proc_cmd.args;
        } catch (const std::exception& e) {
            set_error(ErrorType::RUNTIME_ERROR,
                      cmd_args_value.empty() ? "command" : cmd_args_value[0], e.what(), {});
            last_exit_code = EX_OSERR;
            set_last_pipeline_statuses({EX_OSERR});
            return EX_OSERR;
        }
    }

    auto exec_plan = resolve_command_exec_plan(cmd_args_value, command.is_builtin);
    bool is_builtin = exec_plan.is_builtin;
    std::string cached_exec_path = std::move(exec_plan.cached_exec_path);

    if (auto handler_exit_code = run_command_not_found_handler(cmd_args_value, env_assignments,
                                                               is_builtin, cached_exec_path)) {
        cleanup_process_substitutions(proc_resources, true);
        last_exit_code = *handler_exit_code;
        set_last_pipeline_statuses({*handler_exit_code});
        return *handler_exit_code;
    }

    std::optional<PtyPair> output_pty;
    std::shared_ptr<OutputRelayState> output_relay;
    const bool wants_output_relay = shell_is_interactive && auto_background_on_stop_silent;
    if (wants_output_relay) {
        output_pty = create_output_pty(shell_terminal);
    }

    int launch_barrier[2] = {-1, -1};
    if (monitor_mode && shell_is_interactive && isatty(shell_terminal) != 0) {
        (void)pipe(launch_barrier);
    }

    pid_t pid = fork_command_child();

    if (pid == -1) {
        set_error(ErrorType::RUNTIME_ERROR, cmd_args_value.empty() ? "unknown" : cmd_args_value[0],
                  "failed to fork process: " + std::string(strerror(errno)), {});
        if (output_pty.has_value()) {
            cjsh_filesystem::safe_close(output_pty->master_fd);
            cjsh_filesystem::safe_close(output_pty->slave_fd);
        }
        cjsh_filesystem::safe_close(launch_barrier[0]);
        cjsh_filesystem::safe_close(launch_barrier[1]);
        last_exit_code = EX_OSERR;
        cleanup_process_substitutions(proc_resources, true);
        set_last_pipeline_statuses({EX_OSERR});
        return EX_OSERR;
    }

    if (pid == 0) {
        cjsh_env::apply_env_assignments(env_assignments);

        pid_t child_pid = getpid();
        if (monitor_mode && set_process_group(child_pid, child_pid) < 0) {
            child_exit_with_error(
                ErrorType::RUNTIME_ERROR, cmd_args_value.empty() ? "command" : cmd_args_value[0],
                std::string("setpgid: failed to set process group ID in child: ") +
                    strerror(errno));
        }

        if (launch_barrier[0] >= 0) {
            (void)close(launch_barrier[1]);
            char ready = 0;
            while (read(launch_barrier[0], &ready, 1) < 0 && errno == EINTR) {
            }
            (void)close(launch_barrier[0]);
        }

        reset_child_signals();

        if (output_pty.has_value()) {
            if (dup2(output_pty->slave_fd, STDOUT_FILENO) == -1) {
                child_exit_with_error(
                    ErrorType::RUNTIME_ERROR,
                    cmd_args_value.empty() ? "command" : cmd_args_value[0],
                    std::string("dup2: failed to attach output relay stdout: ") + strerror(errno));
            }
            if (dup2(output_pty->slave_fd, STDERR_FILENO) == -1) {
                child_exit_with_error(
                    ErrorType::RUNTIME_ERROR,
                    cmd_args_value.empty() ? "command" : cmd_args_value[0],
                    std::string("dup2: failed to attach output relay stderr: ") + strerror(errno));
            }
            cjsh_filesystem::safe_close(output_pty->master_fd);
            cjsh_filesystem::safe_close(output_pty->slave_fd);
        }

        exec_builtin_or_external_child(cmd_args_value, is_builtin, cached_exec_path);
    }

    cjsh_filesystem::safe_close(launch_barrier[0]);
    if (monitor_mode && set_process_group(pid, pid) < 0) {
        warn_parent_setpgid_failure();
    }

    Job job = make_single_process_job(pid, args[0], false, auto_background_on_stop,
                                      auto_background_on_stop_silent, monitor_mode);
    job.launch_barrier_fd = launch_barrier[1];
    attach_output_relay_to_job(job, output_pty, output_relay);

    int job_id = add_job(job);

    std::string full_command = join_arguments(args);
    // These arguments are already expanded. Re-parsing their display text can reinterpret
    // literal operators and substitutions; redirections use the pipeline execution path.
    const bool reads_stdin = job_utils::command_consumes_terminal_stdin(proc_cmd);

    int new_job_id = JobManager::instance().add_job(pid, {pid}, full_command, job.background,
                                                    reads_stdin, monitor_mode);
    if (auto managed_job = JobManager::instance().get_job(new_job_id)) {
        managed_job->defer_stop_notification = auto_background_on_stop;
    }

    put_job_in_foreground(job_id, false);

    std::lock_guard<std::mutex> lock(jobs_mutex);
    auto it = jobs.find(job_id);
    int exit_code = last_exit_code;
    std::optional<int> completed_status;

    if (it != jobs.end() && it->second.completed) {
        completed_status = it->second.status;
        exit_code = extract_exit_code(*completed_status);
        JobManager::instance().remove_job(new_job_id);
        (void)jobs.erase(it);
    } else if (it != jobs.end() && it->second.stopped) {
        if (WIFSTOPPED(it->second.status)) {
            exit_code = 128 + WSTOPSIG(it->second.status);
        }
    }

    if (completed_status.has_value()) {
        set_error_from_wait_status(args[0], *completed_status);
    } else {
        auto exit_result = job_utils::make_exit_error_result(
            args[0], exit_code, "command completed successfully", "command failed with exit code ");
        set_error(exit_result.type, args[0], exit_result.message, exit_result.suggestions);
    }
    last_exit_code = exit_code;
    set_last_pipeline_statuses({exit_code});

    cleanup_process_substitutions(proc_resources, false);

    return exit_code;
}

int Exec::execute_command_async(const std::vector<std::string>& args) {
    return execute_prepared_command_async(cjsh_env::prepare_command(args));
}

int Exec::execute_prepared_command_async(cjsh_env::PreparedCommand command) {
    const auto& args = command.original_args;
    if (g_shell) {
        g_shell->mark_terminal_dirty();
    }
    const bool monitor_mode = g_shell && g_shell->is_job_control_enabled();
    if (auto status = handle_prepared_assignments(command, true)) {
        return *status;
    }
    const auto& env_assignments = command.assignments;
    auto& cmd_args_value = command.args;

    auto exec_plan = resolve_command_exec_plan(cmd_args_value, command.is_builtin);
    bool is_builtin = exec_plan.is_builtin;
    std::string cached_exec_path = std::move(exec_plan.cached_exec_path);

    if (auto handler_exit_code = run_command_not_found_handler(cmd_args_value, env_assignments,
                                                               is_builtin, cached_exec_path)) {
        last_exit_code = *handler_exit_code;
        set_last_pipeline_statuses({*handler_exit_code});
        return *handler_exit_code;
    }

    pid_t pid = fork_command_child();

    if (pid == -1) {
        std::string cmd_name = cmd_args_value.empty() ? "unknown" : cmd_args_value[0];
        set_error(ErrorType::RUNTIME_ERROR, cmd_name,
                  "failed to create background process: " + std::string(strerror(errno)), {});
        last_exit_code = EX_OSERR;
        set_last_pipeline_statuses({EX_OSERR});
        return EX_OSERR;
    }

    if (pid == 0) {
        cjsh_env::apply_env_assignments(env_assignments);

        if (monitor_mode && set_process_group(0, 0) < 0) {
            child_exit_with_error(
                ErrorType::RUNTIME_ERROR, cmd_args_value.empty() ? "command" : cmd_args_value[0],
                std::string("setpgid: failed to set process group ID in background child: ") +
                    strerror(errno));
        }

        // POSIX asynchronous lists without job control inherit no terminal input. With monitor
        // mode enabled, the distinct background process group is instead stopped by SIGTTIN if
        // it tries to read the controlling terminal.
        if (!monitor_mode) {
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0) {
                (void)dup2(null_fd, STDIN_FILENO);
                (void)close(null_fd);
            }
        }

        reset_child_signals();

        exec_builtin_or_external_child(cmd_args_value, is_builtin, cached_exec_path);
    } else {
        if (monitor_mode && set_process_group(pid, pid) < 0 && errno != EACCES && errno != EPERM) {
            set_error(ErrorType::RUNTIME_ERROR, "setpgid",
                      "failed to set process group ID for background process: " +
                          std::string(strerror(errno)));
        }

        Job job = make_single_process_job(pid, args[0], true, false, false, monitor_mode);
        job.suppress_notifications =
            g_command_not_found_handler_depth.load(std::memory_order_relaxed) > 0;

        int job_id = add_job(job);

        std::string full_command = join_arguments(args);
        int managed_job_id =
            JobManager::instance().add_job(pid, {pid}, full_command, true, false, monitor_mode);
        if (job.suppress_notifications) {
            auto managed_job = JobManager::instance().get_job(managed_job_id);
            if (managed_job) {
                managed_job->suppress_notifications = true;
            }
        }
        JobManager::instance().set_last_background_pid(pid);

        if (!job.suppress_notifications &&
            (!config::is_posix_mode() || config::interactive_mode || config::force_interactive)) {
            std::cerr << "[" << job_id << "] " << pid << " " << job.command << '\n';
        }
        last_exit_code = 0;
        return 0;
    }
}

int Exec::execute_pipeline(const std::vector<Command>& commands) {
    const bool pipeline_negated = (!commands.empty() && commands[0].negate_pipeline);
    if (g_shell) {
        g_shell->mark_terminal_dirty();
    }
    const bool monitor_mode = g_shell && g_shell->is_job_control_enabled();

    auto apply_pipefail = [&](int exit_code, const std::vector<int>& statuses) -> int {
        if (!g_shell || !g_shell->get_shell_option(ShellOption::Pipefail)) {
            return exit_code;
        }
        if (statuses.empty()) {
            return exit_code;
        }
        int pipefail_exit = 0;
        for (int status : statuses) {
            if (status > 0) {
                pipefail_exit = status;
            }
        }
        return pipefail_exit;
    };

    auto finalize_exit = [&](int exit_code) -> int {
        int effective = exit_code;
        if (pipeline_negated) {
            effective = (exit_code == 0) ? 1 : 0;
        }
        last_exit_code = effective;
        return effective;
    };

    if (commands.empty()) {
        set_error(ErrorType::INVALID_ARGUMENT, "",
                  "cannot execute empty pipeline - no commands provided", {});
        set_last_pipeline_statuses({EX_USAGE});
        return finalize_exit(EX_USAGE);
    }

    if (g_shell && g_shell->get_shell_option(ShellOption::Noexec)) {
        const bool is_background = commands.back().background;
        if (!is_background) {
            set_last_pipeline_statuses(std::vector<int>(commands.size(), 0));
        }
        return finalize_exit(0);
    }

    if (commands.size() == 1) {
        Command cmd = commands[0];
        std::vector<std::pair<std::string, std::string>> env_assignments;
        size_t cmd_start_idx = cjsh_env::collect_env_assignments(cmd.args, env_assignments);
        const size_t original_arg_count = cmd.args.size();

        if (config::is_posix_mode()) {
            for (const auto& [name, value] : env_assignments) {
                if (!readonly_manager_can_assign(name, "assignment")) {
                    return finalize_exit(cjsh_env::posix_error_exit(1));
                }
            }
        }
        if (cmd_start_idx >= original_arg_count) {
            apply_assignments_to_shell_env(env_assignments);
            if (config::is_posix_mode()) {
                const int status =
                    run_with_command_redirections(cmd, [] { return 0; }, "assignment", false);
                set_last_pipeline_statuses({status});
                return finalize_exit(status);
            }
            set_last_pipeline_statuses({0});
            return finalize_exit(0);
        }

        const bool has_temporary_env = strip_temporary_env_assignments(
            cmd.args, cmd_start_idx, original_arg_count, env_assignments);
        const bool assignments_persist = has_temporary_env && !cmd.background &&
                                         !cmd.args.empty() && is_posix_special_builtin(cmd.args[0]);

        if (assignments_persist) {
            apply_assignments_to_shell_env(env_assignments);
        }

        if (can_execute_in_process(cmd)) {
            int exit_code = 0;
            if (assignments_persist) {
                exit_code = execute_builtin_or_special_command(cmd.args);
            } else {
                TemporaryEnvAssignmentScope temp_scope(g_shell.get(), env_assignments);
                exit_code = execute_builtin_or_special_command(cmd.args);
            }
            set_last_pipeline_statuses({exit_code});
            return finalize_exit(exit_code);
        }

        if (cmd.background) {
            int async_result = execute_command_async(commands[0].args);
            if (async_result != 0) {
                set_last_pipeline_statuses({async_result});
            }
            return finalize_exit(async_result);
        }

        ShellScriptInterpreter* interpreter =
            g_shell ? g_shell->get_shell_script_interpreter() : nullptr;
        if (interpreter && !cmd.args.empty() && interpreter->has_function(cmd.args[0])) {
            auto invoke_function = [&] { return interpreter->invoke_function(cmd.args); };
            int function_exit = 0;
            if (requires_fork(cmd)) {
                bool action_invoked = false;
                TemporaryEnvAssignmentScope temp_scope(g_shell.get(), env_assignments);
                function_exit = run_with_command_redirections(cmd, invoke_function, cmd.args[0],
                                                              false, &action_invoked);
            } else {
                TemporaryEnvAssignmentScope temp_scope(g_shell.get(), env_assignments);
                function_exit = invoke_function();
            }
            set_last_pipeline_statuses({function_exit});
            return finalize_exit(function_exit);
        }

        if (is_builtin_or_special_command(cmd.args)) {
            int builtin_exit = 0;
            if (assignments_persist) {
                builtin_exit = execute_builtin_with_redirections(cmd);
            } else {
                TemporaryEnvAssignmentScope temp_scope(g_shell.get(), env_assignments);
                builtin_exit = execute_builtin_with_redirections(cmd);
            }
            set_last_pipeline_statuses({builtin_exit});
            return finalize_exit(builtin_exit);
        }

        ProcessSubstitutionResources proc_resources;
        try {
            proc_resources = setup_process_substitutions(cmd);
        } catch (const std::exception& e) {
            set_error(ErrorType::RUNTIME_ERROR, cmd.args.empty() ? "command" : cmd.args[0],
                      std::string(e.what()));
            set_last_pipeline_statuses({EX_OSERR});
            return finalize_exit(EX_OSERR);
        }

        std::string cached_exec_path;
        if (!cmd.args.empty()) {
            cached_exec_path = cjsh_filesystem::resolve_executable_for_execution(cmd.args[0]);
        }

        if (auto handler_exit_code =
                run_command_not_found_handler(cmd.args, env_assignments, false, cached_exec_path)) {
            cleanup_process_substitutions(proc_resources, true);
            set_last_pipeline_statuses({*handler_exit_code});
            return finalize_exit(*handler_exit_code);
        }

        std::optional<PtyPair> output_pty;
        std::shared_ptr<OutputRelayState> output_relay;
        const bool wants_output_relay = shell_is_interactive && cmd.auto_background_on_stop_silent;
        const bool can_capture_output = wants_output_relay &&
                                        !command_has_stdout_redirection(cmd) &&
                                        !command_has_stderr_redirection(cmd);
        if (can_capture_output) {
            output_pty = create_output_pty(shell_terminal);
        }

        int launch_barrier[2] = {-1, -1};
        if (monitor_mode && shell_is_interactive && isatty(shell_terminal) != 0) {
            (void)pipe(launch_barrier);
        }

        pid_t pid = fork_command_child();

        if (pid == -1) {
            cleanup_process_substitutions(proc_resources, true);
            set_error(ErrorType::RUNTIME_ERROR, cmd.args.empty() ? "unknown" : cmd.args[0],
                      "failed to fork process: " + std::string(strerror(errno)));
            if (output_pty.has_value()) {
                cjsh_filesystem::safe_close(output_pty->master_fd);
                cjsh_filesystem::safe_close(output_pty->slave_fd);
            }
            cjsh_filesystem::safe_close(launch_barrier[0]);
            cjsh_filesystem::safe_close(launch_barrier[1]);
            set_last_pipeline_statuses({EX_OSERR});
            return finalize_exit(EX_OSERR);
        }

        if (pid == 0) {
            const std::string command_name = cmd.args.empty() ? "command" : cmd.args[0];

            if (has_temporary_env) {
                cjsh_env::apply_env_assignments(env_assignments);
            }
            pid_t child_pid = getpid();
            if (monitor_mode && set_process_group(child_pid, child_pid) < 0) {
                child_exit_with_error(
                    ErrorType::RUNTIME_ERROR, command_name,
                    std::string("setpgid: failed to set process group ID in child: ") +
                        strerror(errno));
            }

            if (launch_barrier[0] >= 0) {
                (void)close(launch_barrier[1]);
                char ready = 0;
                while (read(launch_barrier[0], &ready, 1) < 0 && errno == EINTR) {
                }
                (void)close(launch_barrier[0]);
            }

            reset_child_signals();

            if (cmd.redirection_order.empty() && !cmd.here_doc.empty()) {
                auto here_doc_error = [&](HereDocErrorKind kind, const std::string& detail) {
                    child_exit_with_error(
                        ErrorType::RUNTIME_ERROR, command_name,
                        format_here_document_error(kind, detail, HereDocErrorStyle::ChildProcess));
                };

                if (!setup_here_document_stdin(cmd.here_doc, here_doc_error)) {
                    child_exit_with_error(ErrorType::RUNTIME_ERROR, command_name,
                                          "failed to configure here document for stdin");
                }
            } else if (cmd.redirection_order.empty() && !cmd.here_string.empty()) {
                auto here_error = cjsh_filesystem::setup_here_string_stdin(cmd.here_string);
                if (here_error.has_value()) {
                    std::string message;
                    switch (here_error->type) {
                        case cjsh_filesystem::HereStringErrorType::Pipe:
                            message = "pipe: failed to create pipe for here string: " +
                                      here_error->detail;
                            break;
                        case cjsh_filesystem::HereStringErrorType::Write:
                            message =
                                "write: failed to write here string content: " + here_error->detail;
                            break;
                        case cjsh_filesystem::HereStringErrorType::Dup:
                            message = "dup2: failed to duplicate here string descriptor: " +
                                      here_error->detail;
                            break;
                    }
                    child_exit_with_error(ErrorType::RUNTIME_ERROR, command_name, message);
                }
            } else if (cmd.redirection_order.empty() && !cmd.input_file.empty()) {
                auto redirect_result =
                    cjsh_filesystem::redirect_fd(cmd.input_file, STDIN_FILENO, O_RDONLY);
                if (redirect_result.is_error()) {
                    child_exit_with_error(ErrorType::FILE_NOT_FOUND, command_name,
                                          cmd.input_file + ": " + redirect_result.error());
                }
            }

            if (output_pty.has_value()) {
                if (dup2(output_pty->slave_fd, STDOUT_FILENO) == -1) {
                    child_exit_with_error(
                        ErrorType::RUNTIME_ERROR, command_name,
                        std::string("dup2: failed to attach output relay stdout: ") +
                            strerror(errno));
                }
                if (dup2(output_pty->slave_fd, STDERR_FILENO) == -1) {
                    child_exit_with_error(
                        ErrorType::RUNTIME_ERROR, command_name,
                        std::string("dup2: failed to attach output relay stderr: ") +
                            strerror(errno));
                }
                cjsh_filesystem::safe_close(output_pty->master_fd);
                cjsh_filesystem::safe_close(output_pty->slave_fd);
            }

            if (!cmd.redirection_order.empty()) {
                auto ordered_error = [&](ErrorType type, const std::string& message) {
                    child_exit_with_error(type, command_name, message);
                };
                if (!apply_ordered_redirections(cmd, ordered_error)) {
                    _exit(EXIT_FAILURE);
                }
            } else if (!cmd.output_file.empty()) {
                if (cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.output_file,
                                                                        cmd.force_overwrite)) {
                    child_exit_with_error(
                        ErrorType::PERMISSION_DENIED, command_name,
                        cmd.output_file + ": cannot overwrite existing file (noclobber is set)");
                }

                auto redirect_result = cjsh_filesystem::redirect_fd(cmd.output_file, STDOUT_FILENO,
                                                                    O_WRONLY | O_CREAT | O_TRUNC);
                if (redirect_result.is_error()) {
                    child_exit_with_error(ErrorType::FILE_NOT_FOUND, command_name,
                                          cmd.output_file + ": " + redirect_result.error());
                }
            }

            if (cmd.redirection_order.empty() && cmd.both_output && !cmd.both_output_file.empty()) {
                if (cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.both_output_file)) {
                    child_exit_with_error(
                        ErrorType::PERMISSION_DENIED, command_name,
                        cmd.both_output_file +
                            ": cannot overwrite existing file (noclobber is set)");
                }

                auto stdout_result = cjsh_filesystem::redirect_fd(
                    cmd.both_output_file, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
                if (stdout_result.is_error()) {
                    child_exit_with_error(ErrorType::FILE_NOT_FOUND, command_name,
                                          cmd.both_output_file + ": " + stdout_result.error());
                }

                auto stderr_result = cjsh_filesystem::safe_dup2(STDOUT_FILENO, STDERR_FILENO);
                if (stderr_result.is_error()) {
                    child_exit_with_error(
                        ErrorType::RUNTIME_ERROR, command_name,
                        "dup2: failed for stderr in &> redirection: " + stderr_result.error());
                }
            }

            if (cmd.redirection_order.empty() && !cmd.append_file.empty()) {
                int fd = open(cmd.append_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd == -1) {
                    child_exit_with_error(classify_filesystem_error(errno), command_name,
                                          cmd.append_file + ": " + std::string(strerror(errno)));
                }
                if (dup2(fd, STDOUT_FILENO) == -1) {
                    int saved_errno = errno;
                    (void)close(fd);
                    child_exit_with_error(ErrorType::RUNTIME_ERROR, command_name,
                                          "dup2: failed for append redirection: " +
                                              std::string(strerror(saved_errno)));
                }
                (void)close(fd);
            }

            if (cmd.redirection_order.empty() &&
                !configure_stderr_redirects(cmd, handle_stream_redirect_error_and_exit)) {
                _exit(EXIT_FAILURE);
            }

            if (cmd.redirection_order.empty() &&
                !apply_fd_operations(cmd, handle_fd_operation_error_and_exit)) {
                _exit(EXIT_FAILURE);
            }

            const char* exec_override =
                cached_exec_path.empty() ? nullptr : cached_exec_path.c_str();
            exec_external_child(cmd.args, exec_override);
        }

        cjsh_filesystem::safe_close(launch_barrier[0]);

        if (!monitor_mode) {
            cjsh_filesystem::safe_close(launch_barrier[1]);
            // Non-monitor foreground children still need to participate in
            // signal cleanup, without signaling the caller's process group.
            // Negative keys keep this private record out of background job numbering.
            const int foreground_job_id = -pid;
            {
                std::lock_guard<std::mutex> lock(jobs_mutex);
                jobs.emplace(
                    foreground_job_id,
                    make_single_process_job(pid, cmd.args[0], false, cmd.auto_background_on_stop,
                                            cmd.auto_background_on_stop_silent, false));
            }
            const auto process_wait_signals = [&] {
                if (g_shell) {
                    (void)g_shell->process_pending_signals(false);
                } else if (auto* signal_handler = SignalHandler::instance()) {
                    (void)signal_handler->process_pending_signals(this, false);
                }
            };
            process_wait_signals();
            const auto interrupted_exit = [&] {
                cleanup_process_substitutions(proc_resources, false);
                const int code = SignalHandler::termination_signal() != 0
                                     ? 128 + SignalHandler::termination_signal()
                                     : 0;
                set_last_pipeline_statuses({code});
                return finalize_exit(code);
            };
            if (cjsh_env::exit_requested()) {
                return interrupted_exit();
            }
            int status = 0;
            const int wait_options = cmd.auto_background_on_stop ? WUNTRACED : 0;
            pid_t wpid = waitpid(pid, &status, wait_options);
            while (wpid == -1 && errno == EINTR) {
                process_wait_signals();
                if (cjsh_env::exit_requested()) {
                    return interrupted_exit();
                }
                wpid = waitpid(pid, &status, wait_options);
            }
            remove_job(foreground_job_id);

            if (wpid > 0 && cmd.auto_background_on_stop && WIFSTOPPED(status) &&
                WSTOPSIG(status) == SIGTSTP) {
                (void)kill(pid, SIGCONT);

                Job job =
                    make_single_process_job(pid, cmd.args[0], true, cmd.auto_background_on_stop,
                                            cmd.auto_background_on_stop_silent, false);
                int job_id = add_job(job);

                std::string full_command = join_arguments(cmd.args);
                bool reads_stdin = job_utils::command_consumes_terminal_stdin(cmd);
                (void)JobManager::instance().add_job(pid, {pid}, full_command, true, reads_stdin,
                                                     false);
                JobManager::instance().set_last_background_pid(pid);

                std::cerr << "[" << job_id << "] " << pid << " " << full_command << '\n';

                cleanup_process_substitutions(proc_resources, false);
                set_last_pipeline_statuses({0});
                return finalize_exit(0);
            }

            int exit_code = (wpid == -1) ? EX_OSERR : extract_exit_code(status);
            if (wpid == -1) {
                set_error(ErrorType::RUNTIME_ERROR, "waitpid",
                          "failed to wait for child process: " + std::string(strerror(errno)));
            } else {
                set_error_from_wait_status(cmd.args[0], status);
            }
            cleanup_process_substitutions(proc_resources, false);
            set_last_pipeline_statuses({exit_code});
            return finalize_exit(exit_code);
        }

        if (set_process_group(pid, pid) < 0) {
            warn_parent_setpgid_failure();
        }
        Job job = make_single_process_job(pid, cmd.args[0], false, cmd.auto_background_on_stop,
                                          cmd.auto_background_on_stop_silent, true);
        job.launch_barrier_fd = launch_barrier[1];
        attach_output_relay_to_job(job, output_pty, output_relay);

        int job_id = add_job(job);
        std::string full_command = join_arguments(cmd.args);
        bool reads_stdin = job_utils::command_consumes_terminal_stdin(cmd);
        int managed_job_id =
            JobManager::instance().add_job(pid, {pid}, full_command, job.background, reads_stdin);
        if (auto managed_job = JobManager::instance().get_job(managed_job_id)) {
            managed_job->defer_stop_notification = cmd.auto_background_on_stop;
        }
        put_job_in_foreground(job_id, false);

        if ((!cmd.output_file.empty() || !cmd.append_file.empty() || !cmd.stderr_file.empty()) &&
            cjsh_env::shell_variable_is_set("CJSH_FORCE_SYNC")) {
            sync();
        }

        int raw_exit = last_exit_code;
        cleanup_process_substitutions(proc_resources, false);
        {
            std::lock_guard<std::mutex> lock(jobs_mutex);
            auto it = jobs.find(job_id);
            if (it != jobs.end()) {
                set_last_pipeline_statuses(it->second.pipeline_statuses);
                if (it->second.completed) {
                    JobManager::instance().remove_job(managed_job_id);
                    (void)jobs.erase(it);
                }
            } else {
                set_last_pipeline_statuses({raw_exit});
            }
        }
        return finalize_exit(raw_exit);
    }
    std::vector<pid_t> pids;
    pid_t pgid = 0;

    std::vector<std::array<int, 2>> pipes(commands.size() - 1);

    std::optional<PtyPair> output_pty;
    std::shared_ptr<OutputRelayState> output_relay;
    auto close_output_pty = [&] {
        if (output_pty.has_value()) {
            cjsh_filesystem::safe_close(output_pty->master_fd);
            cjsh_filesystem::safe_close(output_pty->slave_fd);
        }
    };
    const bool wants_output_relay =
        shell_is_interactive && commands.back().auto_background_on_stop_silent;
    if (wants_output_relay) {
        bool can_capture_output = !command_has_stdout_redirection(commands.back());
        if (!can_capture_output) {
            for (const auto& cmd : commands) {
                if (!command_has_stderr_redirection(cmd)) {
                    can_capture_output = true;
                    break;
                }
            }
        }
        if (can_capture_output) {
            output_pty = create_output_pty(shell_terminal);
        }
    }

    int launch_barrier[2] = {-1, -1};
    if (monitor_mode && !commands.back().background && shell_is_interactive &&
        isatty(shell_terminal) != 0) {
        (void)pipe(launch_barrier);
    }

    try {
        for (size_t i = 0; i < commands.size() - 1; i++) {
            if (pipe(pipes[i].data()) == -1) {
                set_error(ErrorType::RUNTIME_ERROR, "",
                          "failed to create pipe " + std::to_string(i + 1) +
                              " for pipeline: " + std::string(strerror(errno)));
                set_last_pipeline_statuses({EX_OSERR});
                close_output_pty();
                cjsh_filesystem::safe_close(launch_barrier[0]);
                cjsh_filesystem::safe_close(launch_barrier[1]);
                return finalize_exit(EX_OSERR);
            }
        }

        for (size_t i = 0; i < commands.size(); i++) {
            Command cmd = commands[i];
            std::vector<std::pair<std::string, std::string>> env_assignments;
            size_t cmd_start_idx = cjsh_env::collect_env_assignments(cmd.args, env_assignments);
            const size_t original_arg_count = cmd.args.size();
            const bool has_temporary_env = strip_temporary_env_assignments(
                cmd.args, cmd_start_idx, original_arg_count, env_assignments);

            std::string cached_exec_path;
            if (!cmd.args.empty()) {
                cached_exec_path = cjsh_filesystem::resolve_executable_for_execution(cmd.args[0]);
            }

            if (cmd.args.empty()) {
                set_error(ErrorType::INVALID_ARGUMENT, "",
                          "command " + std::to_string(i + 1) + " in pipeline is empty");
                print_last_error();

                for (size_t j = 0; j < commands.size() - 1; j++) {
                    (void)close(pipes[j][0]);
                    (void)close(pipes[j][1]);
                }

                set_last_pipeline_statuses({1});
                close_output_pty();
                cjsh_filesystem::safe_close(launch_barrier[0]);
                cjsh_filesystem::safe_close(launch_barrier[1]);
                for (pid_t child : pids) {
                    (void)kill(child, SIGTERM);
                }
                return finalize_exit(1);
            }

            pid_t pid = fork_command_child();

            if (pid == -1) {
                std::string cmd_name = cmd.args.empty() ? "unknown" : cmd.args[0];
                set_error(ErrorType::RUNTIME_ERROR, cmd_name,
                          "failed to create process (command " + std::to_string(i + 1) +
                              " in pipeline): " + std::string(strerror(errno)));
                set_last_pipeline_statuses({EX_OSERR});
                close_output_pty();
                cjsh_filesystem::safe_close(launch_barrier[0]);
                cjsh_filesystem::safe_close(launch_barrier[1]);
                for (pid_t child : pids) {
                    (void)kill(child, SIGTERM);
                }
                return finalize_exit(EX_OSERR);
            }

            if (pid == 0) {
                const std::string command_name = cmd.args.empty() ? "exec" : cmd.args[0];
                const auto child_error = [&](ErrorType type, const std::string& message) {
                    child_exit_with_error(type, command_name, message);
                };

                if (monitor_mode && i == 0) {
                    pgid = getpid();
                }

                if (monitor_mode && set_process_group(0, pgid) < 0) {
                    const int saved_errno = errno;
                    child_error(ErrorType::RUNTIME_ERROR, "failed to set process group in child: " +
                                                              std::string(strerror(saved_errno)));
                }

                if (launch_barrier[0] >= 0) {
                    (void)close(launch_barrier[1]);
                    char ready = 0;
                    while (read(launch_barrier[0], &ready, 1) < 0 && errno == EINTR) {
                    }
                    (void)close(launch_barrier[0]);
                }

                reset_child_signals();
                if (has_temporary_env) {
                    cjsh_env::apply_env_assignments(env_assignments);
                }
                if (!monitor_mode && commands.back().background && i == 0 &&
                    job_utils::command_consumes_terminal_stdin(cmd)) {
                    int null_fd = open("/dev/null", O_RDONLY);
                    if (null_fd >= 0) {
                        (void)dup2(null_fd, STDIN_FILENO);
                        (void)close(null_fd);
                    }
                }
                if (i == 0) {
                    if (cmd.redirection_order.empty() && !cmd.here_doc.empty()) {
                        auto here_doc_error = [&](HereDocErrorKind kind,
                                                  const std::string& detail) {
                            child_error(ErrorType::RUNTIME_ERROR,
                                        format_here_document_error(kind, detail));
                        };

                        if (!setup_here_document_stdin(cmd.here_doc, here_doc_error)) {
                            child_error(ErrorType::RUNTIME_ERROR,
                                        "failed to set up here document for pipeline input");
                        }
                    } else if (cmd.redirection_order.empty() && !cmd.input_file.empty()) {
                        int fd = open(cmd.input_file.c_str(), O_RDONLY);
                        if (fd == -1) {
                            const int saved_errno = errno;
                            child_error(classify_filesystem_error(saved_errno),
                                        cmd.input_file + ": " + std::string(strerror(saved_errno)));
                        }
                        if (dup2(fd, STDIN_FILENO) == -1) {
                            const int saved_errno = errno;
                            (void)close(fd);
                            child_error(ErrorType::RUNTIME_ERROR,
                                        std::string("dup2 input failed: ") + strerror(saved_errno));
                        }
                        (void)close(fd);
                    }
                } else {
                    if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                        const int saved_errno = errno;
                        child_error(
                            ErrorType::RUNTIME_ERROR,
                            std::string("dup2 pipe input failed: ") + strerror(saved_errno));
                    }
                }

                if ((output_pty.has_value() && i == commands.size() - 1 &&
                     !command_has_stdout_redirection(cmd)) &&
                    (dup2(output_pty->slave_fd, STDOUT_FILENO) == -1)) {
                    const int saved_errno = errno;
                    child_error(
                        ErrorType::RUNTIME_ERROR,
                        std::string("dup2 output relay stdout failed: ") + strerror(saved_errno));
                }

                if (i == commands.size() - 1) {
                    if (cmd.redirection_order.empty() && !cmd.output_file.empty()) {
                        int fd = open(cmd.output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd == -1) {
                            const int saved_errno = errno;
                            child_error(
                                classify_filesystem_error(saved_errno),
                                cmd.output_file + ": " + std::string(strerror(saved_errno)));
                        }
                        if (dup2(fd, STDOUT_FILENO) == -1) {
                            const int saved_errno = errno;
                            (void)close(fd);
                            child_error(
                                ErrorType::RUNTIME_ERROR,
                                std::string("dup2 output failed: ") + strerror(saved_errno));
                        }
                        (void)close(fd);
                    } else if (cmd.redirection_order.empty() && !cmd.append_file.empty()) {
                        int fd = open(cmd.append_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                        if (fd == -1) {
                            const int saved_errno = errno;
                            child_error(
                                classify_filesystem_error(saved_errno),
                                cmd.append_file + ": " + std::string(strerror(saved_errno)));
                        }
                        if (dup2(fd, STDOUT_FILENO) == -1) {
                            const int saved_errno = errno;
                            (void)close(fd);
                            child_error(
                                ErrorType::RUNTIME_ERROR,
                                std::string("dup2 append failed: ") + strerror(saved_errno));
                        }
                        (void)close(fd);
                    }
                } else {
                    if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                        const int saved_errno = errno;
                        child_error(
                            ErrorType::RUNTIME_ERROR,
                            std::string("dup2 pipe output failed: ") + strerror(saved_errno));
                    }
                }

                if ((output_pty.has_value() && !command_has_stderr_redirection(cmd)) &&
                    (dup2(output_pty->slave_fd, STDERR_FILENO) == -1)) {
                    const int saved_errno = errno;
                    child_error(
                        ErrorType::RUNTIME_ERROR,
                        std::string("dup2 output relay stderr failed: ") + strerror(saved_errno));
                }

                if (!cmd.redirection_order.empty()) {
                    auto ordered_error = [&](ErrorType type, const std::string& message) {
                        child_error(type, message);
                    };
                    if (!apply_ordered_redirections(cmd, ordered_error)) {
                        child_error(ErrorType::RUNTIME_ERROR,
                                    "failed to apply ordered redirections for pipeline child");
                    }
                } else if (!configure_stderr_redirects(cmd,
                                                       handle_stream_redirect_error_and_exit)) {
                    child_error(ErrorType::RUNTIME_ERROR,
                                "failed to configure stderr redirections for pipeline child");
                }

                if (cmd.redirection_order.empty() &&
                    !apply_fd_operations(cmd, handle_fd_operation_error_and_exit)) {
                    child_error(ErrorType::RUNTIME_ERROR,
                                "failed to apply file descriptor operations for pipeline child");
                }

                if (output_pty.has_value()) {
                    cjsh_filesystem::safe_close(output_pty->master_fd);
                    cjsh_filesystem::safe_close(output_pty->slave_fd);
                }

                for (size_t j = 0; j < commands.size() - 1; j++) {
                    (void)close(pipes[j][0]);
                    (void)close(pipes[j][1]);
                }

                ShellScriptInterpreter* interpreter =
                    g_shell ? g_shell->get_shell_script_interpreter() : nullptr;

                if (is_shell_control_structure(cmd)) {
                    int exit_code = 1;
                    if (g_shell) {
                        exit_code = g_shell->execute(command_text_for_interpretation(cmd));
                    }
                    (void)fflush(stdout);
                    (void)fflush(stderr);
                    _exit(exit_code);
                } else if (interpreter && interpreter->has_function(cmd.args[0])) {
                    int exit_code = interpreter->invoke_function(cmd.args);

                    (void)fflush(stdout);
                    (void)fflush(stderr);

                    _exit(exit_code);
                } else if (is_builtin_or_special_command(cmd.args)) {
                    int exit_code = execute_builtin_or_special_command(cmd.args);

                    (void)fflush(stdout);
                    (void)fflush(stderr);

                    _exit(exit_code);
                } else {
                    const char* exec_override =
                        cached_exec_path.empty() ? nullptr : cached_exec_path.c_str();
                    exec_external_child(cmd.args, exec_override);
                }
            }

            if (i == 0) {
                pgid = pid;
            }

            if (monitor_mode && set_process_group(pid, pgid) < 0) {
                if (errno != EACCES && errno != EPERM) {
                    set_error(ErrorType::RUNTIME_ERROR, "setpgid",
                              "failed to set process group ID in pipeline parent: " +
                                  std::string(strerror(errno)));
                }
            }

            pids.push_back(pid);
        }

        for (size_t i = 0; i < commands.size() - 1; i++) {
            (void)close(pipes[i][0]);
            (void)close(pipes[i][1]);
        }
        cjsh_filesystem::safe_close(launch_barrier[0]);

        if (output_pty.has_value()) {
            cjsh_filesystem::safe_close(output_pty->slave_fd);
            output_relay = start_output_relay(output_pty->master_fd, !commands.back().background);
        }

    } catch (const std::exception& e) {
        set_error(ErrorType::RUNTIME_ERROR, "pipeline",
                  "Error executing pipeline: " + std::string(e.what()));
        print_last_error();
        for (pid_t pid : pids) {
            (void)kill(pid, SIGTERM);
        }
        set_last_pipeline_statuses({1});
        close_output_pty();
        cjsh_filesystem::safe_close(launch_barrier[0]);
        cjsh_filesystem::safe_close(launch_barrier[1]);
        return finalize_exit(1);
    }

    Job job;
    job.pgid = pgid;
    job.command = commands[0].args[0] + " | ...";
    job.background = commands.back().background;
    job.auto_background_on_stop = commands.back().auto_background_on_stop;
    job.auto_background_on_stop_silent = commands.back().auto_background_on_stop_silent;
    job.process_group = monitor_mode;
    job.completed = false;
    job.stopped = false;
    job.pids = pids;
    job.last_pid = pids.empty() ? -1 : pids.back();
    job.pid_order = pids;
    job.pipeline_statuses.assign(pids.size(), -1);
    job.output_relay = output_relay;
    job.launch_barrier_fd = launch_barrier[1];

    int job_id = add_job(job);

    std::string pipeline_command;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) {
            pipeline_command += " | ";
        }
        for (size_t j = 0; j < commands[i].args.size(); ++j) {
            if (j > 0) {
                pipeline_command += ' ';
            }
            pipeline_command += commands[i].args[j];
        }
    }
    int new_job_id = JobManager::instance().add_job(
        pgid, pids, pipeline_command, job.background,
        job_utils::pipeline_consumes_terminal_stdin(commands), monitor_mode);
    if (auto managed_job = JobManager::instance().get_job(new_job_id)) {
        managed_job->defer_stop_notification = commands.back().auto_background_on_stop;
    }

    if (job.background) {
        JobManager::instance().set_last_background_pid(pids.empty() ? -1 : pids.back());
    }

    int raw_exit = last_exit_code;

    if (job.background) {
        put_job_in_background(job_id, false);
        if (!config::is_posix_mode() || config::interactive_mode || config::force_interactive) {
            std::cerr << "[" << job_id << "] " << pgid << " " << job.command << '\n';
        }
        raw_exit = 0;
    } else {
        put_job_in_foreground(job_id, false);

        std::lock_guard<std::mutex> lock(jobs_mutex);
        auto it = jobs.find(job_id);
        if (it != jobs.end()) {
            if (it->second.completed) {
                JobManager::instance().remove_job(new_job_id);
                raw_exit = extract_exit_code(it->second.last_status);
                set_last_pipeline_statuses(it->second.pipeline_statuses);
                raw_exit = apply_pipefail(raw_exit, it->second.pipeline_statuses);
                (void)jobs.erase(it);
            } else {
                raw_exit = last_exit_code;
            }
        } else {
            set_last_pipeline_statuses({raw_exit});
        }
    }

    if (job.background) {
        // Leave PIPESTATUS untouched for background pipelines to mirror bash behaviour.
    }

    return finalize_exit(raw_exit);
}

int Exec::run_with_command_redirections(Command cmd, const std::function<int()>& action,
                                        const std::string& command_name, bool persist_fd_changes,
                                        bool* action_invoked) {
    if (action_invoked) {
        *action_invoked = false;
    }

    std::set<int> redirected_fds{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    int highest_fd = 9;
    auto record_fd = [&](int fd, bool redirected) {
        if (fd >= 0) {
            highest_fd = std::max(highest_fd, fd);
            if (redirected) {
                redirected_fds.insert(fd);
            }
        }
    };
    for (const auto& redirection : cmd.redirection_order) {
        record_fd(redirection.fd, true);
        record_fd(redirection.target_fd, false);
    }
    for (const auto& [fd, spec] : cmd.fd_redirections) {
        record_fd(fd, true);
    }
    for (const auto& [fd, source_fd] : cmd.fd_duplications) {
        record_fd(fd, true);
        record_fd(source_fd, false);
    }
    if (highest_fd == std::numeric_limits<int>::max()) {
        set_error(ErrorType::RUNTIME_ERROR, command_name, "file descriptor is too large", {});
        return EX_OSERR;
    }

    // Backups must not occupy any descriptor named by the command, including duplication
    // sources: otherwise saving stdout could make an invalid `1>&10` unexpectedly succeed.
    auto duplicate_fd = [&](int fd) {
        const int min_fd = highest_fd + 1;
        int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
        dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, min_fd);
#endif
        if (dup_fd == -1) {
            dup_fd = fcntl(fd, F_DUPFD, min_fd);
        }
        if ((dup_fd != -1) && (fcntl(dup_fd, F_SETFD, FD_CLOEXEC) == -1)) {
            cjsh_filesystem::safe_close(dup_fd);
            return -1;
        }

        return dup_fd;
    };

    // A loop or function can launch foreground jobs while its redirections are active.
    // Keep the owned terminal handle usable even when the command redirects or closes it,
    // including persistent `exec` redirections.
    if (owns_shell_terminal && redirected_fds.count(shell_terminal) != 0) {
        const int terminal_copy = duplicate_fd(shell_terminal);
        if (terminal_copy == -1) {
            set_error(ErrorType::RUNTIME_ERROR, command_name,
                      "failed to preserve the controlling terminal", {});
            return EX_OSERR;
        }
        cjsh_filesystem::safe_close(shell_terminal);
        shell_terminal = terminal_copy;
    }

    struct SavedDescriptor {
        int fd;
        int backup;
        int flags;
    };
    std::vector<SavedDescriptor> saved_descriptors;
    auto close_backups = [&] {
        for (const auto& saved : saved_descriptors) {
            cjsh_filesystem::safe_close(saved.backup);
        }
    };
    for (int fd : redirected_fds) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags == -1 && errno == EBADF) {
            saved_descriptors.push_back({fd, -1, -1});
            continue;
        }
        const int backup = flags == -1 ? -1 : duplicate_fd(fd);
        if (backup == -1) {
            close_backups();
            set_error(ErrorType::RUNTIME_ERROR, command_name,
                      "failed to save original file descriptors", {});
            return EX_OSERR;
        }
        saved_descriptors.push_back({fd, backup, flags});
    }

    ProcessSubstitutionResources proc_resources;
    auto restore_descriptors = [&](bool terminate_process_subs) {
        cleanup_process_substitutions(proc_resources, terminate_process_subs);

        if (!persist_fd_changes) {
            for (const auto& saved : saved_descriptors) {
                if (saved.backup == -1) {
                    cjsh_filesystem::safe_close(saved.fd);
                } else {
                    (void)cjsh_filesystem::safe_dup2(saved.backup, saved.fd);
                    (void)fcntl(saved.fd, F_SETFD, saved.flags);
                }
            }
        }
        close_backups();
    };

    try {
        proc_resources = setup_process_substitutions(cmd);

        if (!cmd.redirection_order.empty()) {
            auto ordered_error = [&](ErrorType, const std::string& message) {
                throw std::runtime_error(message);
            };
            (void)apply_ordered_redirections(cmd, ordered_error);
        } else if (!cmd.here_doc.empty()) {
            int here_pipe[2] = {-1, -1};
            auto pipe_result = cjsh_filesystem::create_pipe_cloexec(here_pipe);
            if (pipe_result.is_error()) {
                throw std::runtime_error("cjsh: failed to create pipe for here document: " +
                                         pipe_result.error());
            }

            std::string error;
            auto write_result =
                cjsh_filesystem::write_all(here_pipe[1], std::string_view{cmd.here_doc});
            if (write_result.is_error()) {
                if (write_result.error().find("Broken pipe") == std::string::npos &&
                    write_result.error().find("EPIPE") == std::string::npos) {
                    error = write_result.error();
                }

            } else {
                auto newline_result =
                    cjsh_filesystem::write_all(here_pipe[1], std::string_view("\n", 1));
                if (newline_result.is_error() &&
                    (newline_result.error().find("Broken pipe") == std::string::npos &&
                     newline_result.error().find("EPIPE") == std::string::npos)) {
                    error = newline_result.error();
                }
            }

            cjsh_filesystem::safe_close(here_pipe[1]);

            if (!error.empty()) {
                cjsh_filesystem::safe_close(here_pipe[0]);
                throw std::runtime_error("cjsh: failed to write here document content: " + error);
            }

            auto dup_result = cjsh_filesystem::safe_dup2(here_pipe[0], STDIN_FILENO);
            cjsh_filesystem::safe_close(here_pipe[0]);
            if (dup_result.is_error()) {
                throw std::runtime_error("cjsh: failed to redirect stdin for here document: " +
                                         dup_result.error());
            }
        }

        if (cmd.redirection_order.empty() && !cmd.input_file.empty()) {
            auto redirect_result =
                cjsh_filesystem::redirect_fd(cmd.input_file, STDIN_FILENO, O_RDONLY);
            if (redirect_result.is_error()) {
                throw std::runtime_error("cjsh: failed to redirect stdin from " + cmd.input_file +
                                         ": " + redirect_result.error());
            }
        }

        if (cmd.redirection_order.empty() && !cmd.here_string.empty()) {
            auto here_error = cjsh_filesystem::setup_here_string_stdin(cmd.here_string);
            if (here_error.has_value()) {
                switch (here_error->type) {
                    case cjsh_filesystem::HereStringErrorType::Pipe:
                        throw std::runtime_error("failed to create pipe for here string");
                    case cjsh_filesystem::HereStringErrorType::Write:
                        throw std::runtime_error("failed to write here string content");
                    case cjsh_filesystem::HereStringErrorType::Dup:
                        throw std::runtime_error("failed to redirect stdin for here string: " +
                                                 here_error->detail);
                }
            }
        }

        if (cmd.redirection_order.empty() && !cmd.output_file.empty()) {
            if (cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.output_file,
                                                                    cmd.force_overwrite)) {
                throw std::runtime_error("cannot overwrite existing file '" + cmd.output_file +
                                         "' (noclobber is set)");
            }

            auto redirect_result = cjsh_filesystem::redirect_fd(cmd.output_file, STDOUT_FILENO,
                                                                O_WRONLY | O_CREAT | O_TRUNC);
            if (redirect_result.is_error()) {
                throw std::runtime_error("failed to redirect stdout to file '" + cmd.output_file +
                                         "': " + redirect_result.error());
            }
        }

        if (cmd.redirection_order.empty() && cmd.both_output && !cmd.both_output_file.empty()) {
            if (cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.both_output_file)) {
                throw std::runtime_error("cannot overwrite existing file '" + cmd.both_output_file +
                                         "' (noclobber is set)");
            }

            auto stdout_result = cjsh_filesystem::redirect_fd(cmd.both_output_file, STDOUT_FILENO,
                                                              O_WRONLY | O_CREAT | O_TRUNC);
            if (stdout_result.is_error()) {
                throw std::runtime_error("failed to redirect stdout for &>: " +
                                         cmd.both_output_file + ": " + stdout_result.error());
            }

            auto stderr_result = cjsh_filesystem::safe_dup2(STDOUT_FILENO, STDERR_FILENO);
            if (stderr_result.is_error()) {
                throw std::runtime_error("failed to redirect stderr for &>: " +
                                         stderr_result.error());
            }
        }

        if (cmd.redirection_order.empty() && !cmd.append_file.empty()) {
            auto redirect_result = cjsh_filesystem::redirect_fd(cmd.append_file, STDOUT_FILENO,
                                                                O_WRONLY | O_CREAT | O_APPEND);
            if (redirect_result.is_error()) {
                throw std::runtime_error("failed to redirect stdout for append: " +
                                         cmd.append_file + ": " + redirect_result.error());
            }
        }

        if (cmd.redirection_order.empty() && !cmd.stderr_file.empty()) {
            if (!cmd.stderr_append &&
                cjsh_filesystem::should_noclobber_prevent_overwrite(cmd.stderr_file)) {
                throw std::runtime_error("cannot overwrite existing file '" + cmd.stderr_file +
                                         "' (noclobber is set)");
            }

            int flags = O_WRONLY | O_CREAT | (cmd.stderr_append ? O_APPEND : O_TRUNC);
            auto redirect_result =
                cjsh_filesystem::redirect_fd(cmd.stderr_file, STDERR_FILENO, flags);
            if (redirect_result.is_error()) {
                throw std::runtime_error("failed to redirect stderr to file '" + cmd.stderr_file +
                                         "': " + redirect_result.error());
            }
        }

        if (cmd.redirection_order.empty() && cmd.stderr_to_stdout) {
            auto dup_result = cjsh_filesystem::safe_dup2(STDOUT_FILENO, STDERR_FILENO);
            if (dup_result.is_error()) {
                throw std::runtime_error("failed to redirect stderr to stdout: " +
                                         dup_result.error());
            }
        }

        if (cmd.redirection_order.empty() && cmd.stdout_to_stderr) {
            auto dup_result = cjsh_filesystem::safe_dup2(STDERR_FILENO, STDOUT_FILENO);
            if (dup_result.is_error()) {
                throw std::runtime_error("failed to redirect stdout to stderr: " +
                                         dup_result.error());
            }
        }

        if (cmd.redirection_order.empty() &&
            (!cmd.fd_redirections.empty() || !cmd.fd_duplications.empty())) {
            auto fd_error_handler = [&](const FdOperationError& error) -> void {
                switch (error.type) {
                    case FdOperationErrorType::Redirect:
                        throw std::runtime_error(error.spec + ": " + error.error);
                    case FdOperationErrorType::Duplication:
                        throw std::runtime_error("dup2 failed for " + std::to_string(error.fd_num) +
                                                 ">&" + std::to_string(error.src_fd) + ": " +
                                                 error.error);
                }
            };
            (void)apply_fd_operations(cmd, fd_error_handler);
        }

        (void)std::cout.flush();
        (void)std::cerr.flush();
        (void)std::clog.flush();

        int exit_code = action();
        if (action_invoked) {
            *action_invoked = true;
        }

        (void)std::cout.flush();
        (void)std::cerr.flush();
        (void)std::clog.flush();

        restore_descriptors(false);
        return exit_code;
    } catch (const std::exception& e) {
        restore_descriptors(true);
        set_error(ErrorType::RUNTIME_ERROR, command_name, std::string(e.what()));
        return EX_OSERR;
    }
}

namespace exec_utils {

namespace {

CommandOutput execute_with_stdout_capture_impl(const std::function<int()>& child_executor,
                                               bool capture_stderr, bool suppress_stderr,
                                               const std::function<void()>& progress_callback,
                                               unsigned int progress_interval_ms,
                                               const std::function<bool()>& cancellation_callback) {
    if (g_shell) {
        g_shell->mark_terminal_dirty();
    }
    CommandOutput result{"", -1, false};

    if (!child_executor) {
        return result;
    }

    int pipefd[2];
    auto pipe_result = cjsh_filesystem::create_pipe_cloexec(pipefd);
    if (pipe_result.is_error()) {
        return result;
    }

    flush_standard_streams_before_fork();

    pid_t pid = fork();
    if (pid == -1) {
        cjsh_filesystem::close_pipe(pipefd);
        return result;
    }

    if (pid == 0) {
        cjsh_filesystem::safe_close(pipefd[0]);
        (void)setpgid(0, 0);
        if (g_shell) {
            // Captured commands must keep descendants in this private group so
            // cancellation reaches them even in an interactive parent shell.
            (void)g_shell->set_job_control_enabled(false);
        }

        auto dup_result = cjsh_filesystem::safe_dup2(pipefd[1], STDOUT_FILENO);
        if (dup_result.is_error()) {
            _exit(127);
        }

        if (capture_stderr) {
            auto stderr_dup_result = cjsh_filesystem::safe_dup2(pipefd[1], STDERR_FILENO);
            if (stderr_dup_result.is_error()) {
                _exit(127);
            }
        } else if (suppress_stderr) {
            auto devnull_result = cjsh_filesystem::safe_open("/dev/null", O_WRONLY);
            if (devnull_result.is_ok()) {
                (void)cjsh_filesystem::safe_dup2(devnull_result.value(), STDERR_FILENO);
                cjsh_filesystem::safe_close(devnull_result.value());
            }
        }

        cjsh_filesystem::safe_close(pipefd[1]);

        int exit_code = child_executor();
        _exit(exit_code);
    }

    // Keep the executor and any descendants in a private process group so a
    // cancellation cannot leave grandchildren holding the capture pipe open.
    (void)setpgid(pid, pid);
    cjsh_filesystem::safe_close(pipefd[1]);

    int status = 0;
    const bool startup_cancellable = config::interactive_mode && cjsh_env::startup_active() &&
                                     !SignalHandler::is_forked_child() &&
                                     !SignalHandler::executing_trap();
    if (!progress_callback && !startup_cancellable) {
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            result.output.append(buffer, static_cast<size_t>(bytes_read));
        }
        cjsh_filesystem::safe_close(pipefd[0]);
        if (waitpid(pid, &status, 0) == -1) {
            return result;
        }
    } else {
        const int interval_ms =
            progress_callback
                ? static_cast<int>(std::max(1U, std::min(progress_interval_ms, 60000U)))
                : 20;
        auto last_progress = std::chrono::steady_clock::now();
        bool pipe_open = true;
        bool child_reaped = false;
        bool io_failed = false;
        bool cancellation_requested = false;

        while (pipe_open || !child_reaped) {
            if ((!child_reaped && cancellation_callback && cancellation_callback()) ||
                (!cancellation_requested && startup_cancellable &&
                 SignalHandler::startup_interrupted())) {
                cancellation_requested = true;
                if (kill(-pid, SIGINT) < 0 && !child_reaped) {
                    (void)kill(pid, SIGINT);
                }
            }

            pollfd descriptor{pipefd[0], static_cast<short>(POLLIN | POLLHUP | POLLERR), 0};
            int poll_result =
                poll(pipe_open ? &descriptor : nullptr, pipe_open ? 1 : 0, interval_ms);
            if (poll_result < 0 && errno != EINTR) {
                io_failed = true;
                break;
            }
            if (pipe_open && poll_result > 0 && (descriptor.revents & POLLNVAL) != 0) {
                io_failed = true;
                break;
            }

            if (pipe_open && poll_result > 0 &&
                (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                char buffer[4096];
                ssize_t bytes_read = read(pipefd[0], buffer, sizeof(buffer));
                if (bytes_read > 0) {
                    result.output.append(buffer, static_cast<size_t>(bytes_read));
                } else if (bytes_read == 0) {
                    cjsh_filesystem::safe_close(pipefd[0]);
                    pipe_open = false;
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    io_failed = true;
                    break;
                }
            }

            if (!child_reaped) {
                pid_t waited = waitpid(pid, &status, WNOHANG);
                if (waited == pid) {
                    child_reaped = true;
                } else if (waited < 0 && errno != EINTR) {
                    io_failed = true;
                    break;
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (!child_reaped &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress)
                        .count() >= interval_ms) {
                if (!cancellation_requested && progress_callback) {
                    progress_callback();
                }
                last_progress = now;
            }
        }

        if (pipe_open) {
            cjsh_filesystem::safe_close(pipefd[0]);
        }
        if (!child_reaped) {
            while (waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR) {
                    io_failed = true;
                    break;
                }
            }
        }
        if (io_failed) {
            return result;
        }
    }

    result.exit_code = extract_exit_code(status);
    result.success = (result.exit_code == 0);
    return result;
}

CommandOutput execute_args_for_output_impl(
    const std::vector<std::string>& args, const std::function<void()>& progress_callback,
    unsigned int progress_interval_ms, const std::function<bool()>& cancellation_callback = {}) {
    if (args.empty()) {
        return {"", -1, false};
    }

    std::string cached_exec_path = cjsh_filesystem::resolve_executable_for_execution(args[0]);
    return execute_with_stdout_capture_impl(
        [&]() -> int {
            const char* exec_override =
                cached_exec_path.empty() ? nullptr : cached_exec_path.c_str();
            exec_external_child(args, exec_override);
            return 127;
        },
        false, true, progress_callback, progress_interval_ms, cancellation_callback);
}

}  // namespace

CommandOutput execute_with_stdout_capture(const std::function<int()>& child_executor,
                                          bool capture_stderr, bool suppress_stderr) {
    return execute_with_stdout_capture_impl(child_executor, capture_stderr, suppress_stderr, {}, 0,
                                            {});
}

CommandOutput execute_command_for_output(const std::string& command) {
    std::vector<std::string> args = cjsh_env::parse_shell_command(command);
    return execute_args_for_output_impl(args, {}, 0);
}

CommandOutput execute_command_vector_for_output(const std::vector<std::string>& args) {
    return execute_args_for_output_impl(args, {}, 0);
}

CommandOutput execute_command_vector_for_output_with_progress(
    const std::vector<std::string>& args, const std::function<void()>& progress_callback,
    unsigned int progress_interval_ms, const std::function<bool()>& cancellation_callback) {
    return execute_args_for_output_impl(args, progress_callback, progress_interval_ms,
                                        cancellation_callback);
}

}  // namespace exec_utils
