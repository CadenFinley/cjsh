/*
  job_control_commands.cpp

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

#include "job_control_commands.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "builtin_help.h"
#include "builtin_option_parser.h"
#include "error_out.h"
#include "exec.h"
#include "job_control.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "wait_status_utils.h"

namespace {

std::optional<job_control_helpers::ResolvedJob> resolve_updated_control_job(
    const std::vector<std::string>& args, JobManager& job_manager) {
    job_manager.update_job_statuses();
    return job_control_helpers::resolve_control_job_target(args, job_manager);
}

bool require_monitor_mode(const char* command) {
    if (shell && shell->is_job_control_enabled()) {
        return true;
    }
    print_error({ErrorType::RUNTIME_ERROR, command, "no job control", {"Use 'set -m' first"}});
    return false;
}

bool signal_job_processes(const std::shared_ptr<JobControlJob>& job, int signal) {
    if (!job) {
        errno = ESRCH;
        return false;
    }
    if (job->process_group && job->pgid > 0) {
        return killpg(job->pgid, signal) == 0;
    }

    bool sent = false;
    int last_error = ESRCH;
    for (pid_t pid : job->remaining_pids) {
        if (pid > 0 && kill(pid, signal) == 0) {
            sent = true;
        } else {
            last_error = errno;
        }
    }
    if (!sent) {
        errno = last_error;
    }
    return sent;
}

int open_controlling_terminal(bool& should_close) {
    should_close = false;
#ifdef O_CLOEXEC
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
#else
    int fd = open("/dev/tty", O_RDWR);
#endif
    if (fd >= 0) {
        should_close = true;
        return fd;
    }
    return isatty(STDIN_FILENO) != 0 ? STDIN_FILENO : -1;
}

}  // namespace

int suspend_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: suspend [-f]", "Suspend this interactive shell until continued.",
                   "-f allows suspension of a login shell."})) {
        return 0;
    }
    const bool force = args.size() == 2 && args[1] == "-f";
    if (args.size() > 1 && !force) {
        print_error({ErrorType::INVALID_ARGUMENT, "suspend", "Usage: suspend [-f]", {}});
        return 2;
    }
    if (config::login_mode && !force) {
        print_error(
            {ErrorType::RUNTIME_ERROR, "suspend", "cannot suspend a login shell; use -f", {}});
        return 1;
    }
    if (!config::interactive_mode || !shell || !shell->suspend()) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "suspend",
                     "requires an interactive shell with a controlling terminal",
                     {}});
        return 1;
    }
    return 0;
}

int bg_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: bg [JOB_SPEC|PID ...]", "Resume stopped jobs in the background."})) {
        return 0;
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();

    if (!shell || !shell->is_job_control_enabled()) {
        if (job_manager.get_all_jobs().empty()) {
            (void)job_control_helpers::resolve_control_job_target({"bg"}, job_manager);
            return 1;
        }
        (void)require_monitor_mode("bg");
        return 1;
    }

    std::vector<std::vector<std::string>> lookups;
    if (args.size() == 1) {
        lookups.push_back({"bg"});
    } else {
        for (size_t i = 1; i < args.size(); ++i) {
            lookups.push_back({"bg", args[i]});
        }
    }

    bool had_error = false;
    for (const auto& lookup : lookups) {
        auto resolved_job = job_control_helpers::resolve_control_job_target(lookup, job_manager);
        if (!resolved_job) {
            had_error = true;
            continue;
        }

        auto job = resolved_job->job;
        const int job_id = resolved_job->job_id;
        if (job->state.load(std::memory_order_relaxed) != JobState::STOPPED) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "bg",
                         std::to_string(job_id) + ": job already running",
                         {"Use 'jobs' to list job states"}});
            had_error = true;
            continue;
        }

        if (shell && shell->executor) {
            shell->executor->set_job_output_forwarding(job->pgid, false);
        }
        if (!signal_job_processes(job, SIGCONT)) {
            print_error_errno({ErrorType::RUNTIME_ERROR, "bg", "SIGCONT", {}});
            had_error = true;
            continue;
        }

        job->state.store(JobState::RUNNING, std::memory_order_relaxed);
        job->stopped_pids.clear();
        job->stop_signal = 0;
        job->stop_notified.store(false, std::memory_order_relaxed);
        job->background.store(true, std::memory_order_relaxed);
        job->notified = false;
        job_manager.set_current_job(job_id);
        std::cout << "[" << job_id << "]+ " << job->display_command() << " &" << '\n';
    }

    return had_error ? 1 : 0;
}

int fg_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(args, {"Usage: fg [JOB_SPEC|PID]", "Bring a job to the foreground."})) {
        return 0;
    }

    if (args.size() > 2) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "fg",
                     "too many arguments",
                     {"Usage: fg [JOB_SPEC|PID]"}});
        return 1;
    }
    auto& job_manager = JobManager::instance();
    auto resolved_job = resolve_updated_control_job(args, job_manager);
    if (!resolved_job) {
        return 1;
    }
    if (!require_monitor_mode("fg")) {
        return 1;
    }

    auto job = resolved_job->job;
    int job_id = resolved_job->job_id;

    auto consume_completed_job = [&]() -> std::optional<int> {
        const JobState state = job->state.load(std::memory_order_relaxed);
        if (state != JobState::DONE && state != JobState::TERMINATED) {
            return std::nullopt;
        }

        job_manager.notify_job_finished(job);
        const int exit_status = job->exit_status;
        job_manager.remove_job(job_id);
        return exit_status;
    };

    if (consume_completed_job().has_value()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "fg",
                     std::to_string(job_id) + ": job has already completed",
                     {"Use 'jobs' to list available jobs"}});
        return 1;
    }

    bool close_terminal = false;
    const int terminal_fd =
        shell && shell->manages_terminal() ? open_controlling_terminal(close_terminal) : -1;
    bool terminal_control_acquired = false;
    struct termios shell_modes{};
    const bool shell_modes_saved = terminal_fd >= 0 && tcgetattr(terminal_fd, &shell_modes) == 0;
    auto restore_terminal_control = [&](bool restore_modes = true) {
        if (terminal_control_acquired) {
            (void)tcsetpgrp(terminal_fd, getpgrp());
            if (restore_modes && shell_modes_saved) {
                (void)tcsetattr(terminal_fd, TCSADRAIN, &shell_modes);
            }
            terminal_control_acquired = false;
        }
        if (close_terminal) {
            (void)close(terminal_fd);
            close_terminal = false;
        }
    };

    if (terminal_fd >= 0 && job->process_group) {
        if (tcsetpgrp(terminal_fd, job->pgid) < 0) {
            const int tcsetpgrp_error = errno;
            job_manager.update_job_statuses();
            if (auto exit_status = consume_completed_job()) {
                restore_terminal_control();
                return *exit_status;
            }
            errno = tcsetpgrp_error;
            print_error_errno({ErrorType::RUNTIME_ERROR, "fg", "tcsetpgrp", {}});
            restore_terminal_control();
            return 1;
        }
        terminal_control_acquired = true;
        if (job->tmodes_saved) {
            (void)tcsetattr(terminal_fd, TCSADRAIN, &job->tmodes);
        }
    }

    if (shell && shell->executor) {
        shell->executor->set_job_output_forwarding(job->pgid, true);
    }

    if (job->state.load(std::memory_order_relaxed) == JobState::STOPPED &&
        !signal_job_processes(job, SIGCONT)) {
        const int killpg_error = errno;
        restore_terminal_control();
        job_manager.update_job_statuses();
        if (auto exit_status = consume_completed_job()) {
            return *exit_status;
        }
        errno = killpg_error;
        print_error_errno({ErrorType::RUNTIME_ERROR, "fg", "SIGCONT", {}});
        return 1;
    }

    job->state.store(JobState::RUNNING, std::memory_order_relaxed);
    job->stop_notified.store(false, std::memory_order_relaxed);
    job->background.store(false, std::memory_order_relaxed);
    job->notified = false;
    job_manager.set_current_job(job_id);

    std::cout << job->display_command() << '\n';

    auto exit_status = job_control_helpers::wait_for_job(job, job_manager, true);

    if (terminal_control_acquired &&
        job->state.load(std::memory_order_relaxed) == JobState::STOPPED &&
        tcgetattr(terminal_fd, &job->tmodes) == 0) {
        job->tmodes_saved = true;
    }

    const JobState final_state = job->state.load(std::memory_order_relaxed);
    restore_terminal_control(final_state != JobState::DONE);
    if (final_state == JobState::STOPPED) {
        job_manager.notify_job_stopped(job);
        return exit_status.value_or(128 + (job->stop_signal > 0 ? job->stop_signal : SIGTSTP));
    }
    if (final_state == JobState::DONE || final_state == JobState::TERMINATED) {
        job_manager.notify_job_finished(job);
        job_manager.remove_job(job_id);
        return exit_status.value_or(job->exit_status);
    }

    return exit_status.value_or(1);
}

int jobs_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: jobs [-lprs] [JOB_SPEC|PID ...]", "List jobs.",
                   "  -l  Show process-group leaders and status",
                   "  -p  Print process-group leaders only", "  -r  Show running jobs only",
                   "  -s  Show stopped jobs only"})) {
        return 0;
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();

    bool long_format = false;
    bool pid_only = false;
    bool running_only = false;
    bool stopped_only = false;

    size_t start_index = 1;
    const bool options_ok =
        builtin_parse_short_options(args, start_index, "jobs", [&](char option) {
            switch (option) {
                case 'l':
                    long_format = true;
                    return true;
                case 'p':
                    pid_only = true;
                    return true;
                case 'r':
                    running_only = true;
                    return true;
                case 's':
                    stopped_only = true;
                    return true;
                default:
                    return false;
            }
        });
    if (!options_ok) {
        return 1;
    }
    auto jobs = job_manager.get_all_jobs();
    bool had_error = false;
    if (start_index < args.size()) {
        jobs.clear();
        std::unordered_set<int> selected_ids;
        for (size_t i = start_index; i < args.size(); ++i) {
            auto selected =
                job_control_helpers::resolve_control_job_target({"jobs", args[i]}, job_manager);
            if (!selected) {
                had_error = true;
                continue;
            }
            if (selected_ids.insert(selected->job_id).second) {
                jobs.push_back(selected->job);
            }
        }
    }

    if (jobs.empty()) {
        if (!pid_only && !had_error) {
            std::cout << "No jobs" << '\n';
        }
        return had_error ? 1 : 0;
    }

    int current = job_manager.get_current_job();
    int previous = job_manager.get_previous_job();

    for (const auto& job : jobs) {
        const JobState state = job->state.load(std::memory_order_relaxed);
        if ((running_only || stopped_only) && (!running_only || state != JobState::RUNNING) &&
            (!stopped_only || state != JobState::STOPPED)) {
            continue;
        }

        if (pid_only) {
            std::cout << (job->process_group ? job->pgid : job->last_pid) << '\n';
            continue;
        }

        std::string status_char = " ";
        if (job->job_id == current) {
            status_char = "+";
        } else if (job->job_id == previous) {
            status_char = "-";
        }

        std::string state_str;
        switch (state) {
            case JobState::RUNNING:
                state_str = "Running";
                break;
            case JobState::STOPPED:
                state_str = "Stopped";
                break;
            case JobState::DONE:
                state_str = "Done";
                break;
            case JobState::TERMINATED:
                state_str = "Terminated";
                break;
        }

        std::cout << "[" << job->job_id << "]" << status_char << " ";

        if (long_format) {
            std::cout << std::setw(8) << (job->process_group ? job->pgid : job->pids.front())
                      << " ";
        }

        std::cout << std::setw(12) << std::left << state_str << " " << job->display_command()
                  << '\n';

        if (state == JobState::DONE || state == JobState::TERMINATED) {
            job->notified = true;
        }
    }

    return had_error ? 1 : 0;
}

int wait_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(args,
                            {"Usage: wait [-fn] [-p VARNAME] [ID ...]",
                             "Wait for specified jobs or processes. Without IDs, wait for all.",
                             "  -f          Wait for termination instead of a stop",
                             "  -n          Wait for the next selected job or process",
                             "  -p VARNAME  Store the waited job or process ID"})) {
        return 0;
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();

    bool wait_next = false;
    bool force_completion = false;
    std::string result_variable;
    size_t operand_index = 1;
    for (; operand_index < args.size(); ++operand_index) {
        const std::string& arg = args[operand_index];
        if (arg == "--") {
            ++operand_index;
            break;
        }
        if (arg == "-n") {
            wait_next = true;
            continue;
        }
        if (arg == "-f") {
            force_completion = true;
            continue;
        }
        if (arg == "-p" || arg.rfind("-p", 0) == 0) {
            if (arg == "-p") {
                if (++operand_index >= args.size()) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 "wait",
                                 "-p requires a variable name",
                                 {"Usage: wait [-fn] [-p VARNAME] [ID ...]"}});
                    return 2;
                }
                result_variable = args[operand_index];
            } else {
                result_variable = arg.substr(2);
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "wait",
                         "invalid option: " + arg,
                         {"Usage: wait [-fn] [-p VARNAME] [ID ...]"}});
            return 2;
        }
        break;
    }

    if (!result_variable.empty()) {
        const bool valid_name =
            (std::isalpha(static_cast<unsigned char>(result_variable[0])) != 0 ||
             result_variable[0] == '_') &&
            std::all_of(result_variable.begin() + 1, result_variable.end(),
                        [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
        if (!valid_name) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "wait",
                         "'" + result_variable + "' is not a valid variable name",
                         {}});
            return 2;
        }
        (void)cjsh_env::unset_shell_variable_value(result_variable);
    }

    struct WaitTarget {
        std::shared_ptr<JobControlJob> job;
        pid_t pid{-1};
        std::string operand;
    };
    std::vector<WaitTarget> targets;
    bool target_error = false;

    for (size_t i = operand_index; i < args.size(); ++i) {
        const std::string& operand = args[i];
        if (!operand.empty() && operand[0] == '%') {
            auto resolved =
                job_control_helpers::resolve_control_job_target({"wait", operand}, job_manager);
            if (!resolved) {
                target_error = true;
                continue;
            }
            targets.push_back({resolved->job, -1, operand});
            continue;
        }

        auto parsed_pid = job_control_helpers::parse_pid_specifier(operand);
        if (!parsed_pid || *parsed_pid <= 0) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "wait",
                         operand + ": expected a process ID or job specification",
                         {"Use 'jobs' to list available jobs"}});
            target_error = true;
            continue;
        }
        if (auto job = job_manager.get_job_by_pid_or_pgid(*parsed_pid)) {
            targets.push_back({job, -1, operand});
        } else {
            targets.push_back({nullptr, *parsed_pid, operand});
        }
    }

    if (target_error) {
        return 127;
    }

    const bool had_operands = operand_index < args.size();
    if (!had_operands) {
        for (const auto& job : job_manager.get_all_jobs()) {
            targets.push_back({job, -1, {}});
        }
    }

    for (const auto& target : targets) {
        if (target.job) {
            target.job->suppress_notifications = true;
        }
    }

    auto finish_job = [&](const std::shared_ptr<JobControlJob>& job, int status) {
        if (!job) {
            return;
        }
        const JobState state = job->state.load(std::memory_order_relaxed);
        if (state == JobState::DONE || state == JobState::TERMINATED) {
            for (pid_t pid : job->pids) {
                (void)job_manager.consume_completed_pid_status(pid);
            }
            job_manager.remove_job(job->job_id);
        }
        (void)status;
    };

    auto publish_waited_id = [&](pid_t id) {
        if (!result_variable.empty() && id > 0) {
            (void)cjsh_env::set_shell_variable_value(result_variable, std::to_string(id));
        }
    };

    if (wait_next) {
        if (targets.empty()) {
            return 127;
        }

        for (;;) {
            for (const auto& target : targets) {
                if (target.job) {
                    const JobState state = target.job->state.load(std::memory_order_relaxed);
                    if (state == JobState::DONE || state == JobState::TERMINATED ||
                        (!force_completion && state == JobState::STOPPED)) {
                        const int status =
                            state == JobState::STOPPED
                                ? 128 + (target.job->stop_signal > 0 ? target.job->stop_signal
                                                                     : SIGTSTP)
                                : target.job->exit_status;
                        publish_waited_id(target.job->pgid);
                        finish_job(target.job, status);
                        return status;
                    }
                } else if (auto cached = job_manager.consume_completed_pid_status(target.pid)) {
                    publish_waited_id(target.pid);
                    return *cached;
                }
            }

            int status = 0;
            pid_t pid = waitpid(-1, &status, WUNTRACED | WCONTINUED);
            if (pid < 0) {
                if (errno == EINTR) {
                    if (shell) {
                        (void)shell->process_pending_signals();
                    }
                    continue;
                }
                return 127;
            }

            auto changed_job = job_manager.get_job_by_pid(pid);
            if (shell && shell->executor) {
                shell->executor->handle_child_signal(pid, status);
            }
            job_manager.handle_child_status(pid, status);

            bool selected = false;
            for (const auto& target : targets) {
                selected =
                    (target.job && target.job == changed_job) || (!target.job && target.pid == pid);
                if (selected) {
                    break;
                }
            }
            if (!selected || WIFCONTINUED(status) || (force_completion && WIFSTOPPED(status))) {
                continue;
            }

            if (changed_job) {
                const JobState state = changed_job->state.load(std::memory_order_relaxed);
                if (state != JobState::DONE && state != JobState::TERMINATED &&
                    state != JobState::STOPPED) {
                    continue;
                }
                const int result =
                    state == JobState::STOPPED ? 128 + WSTOPSIG(status) : changed_job->exit_status;
                publish_waited_id(changed_job->pgid);
                finish_job(changed_job, result);
                return result;
            }

            publish_waited_id(pid);
            return wait_status_utils::to_exit_code(status);
        }
    }

    int last_exit_status = 0;
    for (const auto& target : targets) {
        if (target.job) {
            pid_t status_pid = -1;
            auto result = job_control_helpers::wait_for_job(target.job, job_manager,
                                                            !force_completion, &status_pid);
            if (!result) {
                print_error({ErrorType::RUNTIME_ERROR,
                             "wait",
                             target.operand + ": not a child of this shell",
                             {}});
                return 127;
            }
            last_exit_status = *result;
            publish_waited_id(target.job->pgid);
            finish_job(target.job, last_exit_status);
            continue;
        }

        if (auto cached = job_manager.consume_completed_pid_status(target.pid)) {
            last_exit_status = *cached;
            publish_waited_id(target.pid);
            continue;
        }

        int status = 0;
        const int options = force_completion ? 0 : WUNTRACED;
        pid_t waited = waitpid(target.pid, &status, options);
        while (waited < 0 && errno == EINTR) {
            if (shell) {
                (void)shell->process_pending_signals();
            }
            waited = waitpid(target.pid, &status, options);
        }
        if (waited < 0) {
            if (auto cached = job_manager.consume_completed_pid_status(target.pid)) {
                last_exit_status = *cached;
                publish_waited_id(target.pid);
                continue;
            }
            print_error({ErrorType::RUNTIME_ERROR,
                         "wait",
                         target.operand + ": not a child of this shell",
                         {}});
            return 127;
        }
        last_exit_status =
            WIFSTOPPED(status) ? 128 + WSTOPSIG(status) : wait_status_utils::to_exit_code(status);
        job_manager.handle_child_status(waited, status);
        publish_waited_id(waited);
    }

    // Bash/POSIX define operand-less wait as successful once all waitable jobs have been
    // consumed, irrespective of the individual job statuses.
    return had_operands ? last_exit_status : 0;
}

int disown_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args,
            {"Usage: disown [-arh] [JOB_SPEC|PID ...]", "       disown [--all|--running] [-h]",
             "Remove jobs, or mark them to be excluded from SIGHUP.",
             "  -a, --all      Select every job", "  -r, --running  Select running jobs only",
             "  -h             Keep jobs but suppress SIGHUP"})) {
        return 0;
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();

    bool disown_all = false;
    bool running_only = false;
    bool mark_hup_only = false;
    std::vector<std::string> operands;
    bool parse_options = true;

    for (size_t i = 1; i < args.size(); ++i) {
        if (parse_options && args[i] == "--") {
            parse_options = false;
            continue;
        }
        if (parse_options && (args[i] == "--all" || args[i] == "--running")) {
            if (args[i] == "--all") {
                disown_all = true;
            } else {
                running_only = true;
            }
            continue;
        }
        if (parse_options && args[i].size() > 1 && args[i][0] == '-') {
            bool valid = true;
            for (size_t j = 1; j < args[i].size(); ++j) {
                switch (args[i][j]) {
                    case 'a':
                        disown_all = true;
                        break;
                    case 'r':
                        running_only = true;
                        break;
                    case 'h':
                        mark_hup_only = true;
                        break;
                    default:
                        valid = false;
                        break;
                }
            }
            if (!valid) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "disown",
                             "invalid option: " + args[i],
                             {"Usage: disown [-arh] [JOB_SPEC|PID ...]"}});
                return 1;
            }
            continue;
        }
        operands.push_back(args[i]);
    }

    std::vector<std::shared_ptr<JobControlJob>> targets;
    std::unordered_set<int> target_ids;
    auto add_target = [&](const std::shared_ptr<JobControlJob>& job) {
        if (job && target_ids.insert(job->job_id).second) {
            targets.push_back(job);
        }
    };

    if (disown_all || (running_only && operands.empty())) {
        for (const auto& job : job_manager.get_all_jobs()) {
            if (!running_only || job->state.load(std::memory_order_relaxed) == JobState::RUNNING) {
                add_target(job);
            }
        }
    } else if (!operands.empty()) {
        bool resolution_error = false;
        for (const auto& operand : operands) {
            auto resolved =
                job_control_helpers::resolve_control_job_target({"disown", operand}, job_manager);
            if (!resolved) {
                resolution_error = true;
                continue;
            }
            if (!running_only ||
                resolved->job->state.load(std::memory_order_relaxed) == JobState::RUNNING) {
                add_target(resolved->job);
            }
        }
        if (resolution_error) {
            return 1;
        }
    } else {
        auto resolved = job_control_helpers::resolve_control_job_target({"disown"}, job_manager);
        if (!resolved) {
            return 1;
        }
        add_target(resolved->job);
    }

    if (targets.empty()) {
        if (disown_all || running_only) {
            return 0;
        }
        print_error({ErrorType::INVALID_ARGUMENT,
                     "disown",
                     "no current job",
                     {"Use 'jobs' to identify targets"}});
        return 1;
    }

    for (const auto& job : targets) {
        if (mark_hup_only) {
            job->hup_protected = true;
            if (shell && shell->executor) {
                shell->executor->set_job_hup_protected(job->pgid, true);
            }
            continue;
        }

        const int job_id = job->job_id;
        const pid_t pgid = job->pgid;
        job_manager.remove_job(job_id);
        if (shell && shell->executor) {
            shell->executor->remove_job_by_pgid(pgid);
        }
    }

    return 0;
}

namespace {

bool is_blank(const std::string& value) {
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::isspace(ch); });
}

std::string normalize_name(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return {};
    }
    return string_utils::join_strings(args, " ", 2);
}

std::string kill_signal_list_text() {
    std::ostringstream out;
    const auto& signals = SignalHandler::available_signals();
    bool first = true;
    for (const auto& signal : signals) {
        if (signal.name == nullptr || signal.signal <= 0) {
            continue;
        }
        if (!first) {
            out << ' ';
        }
        first = false;
        out << SignalHandler::signal_to_name(signal.signal, true);
    }
    return out.str();
}

}  // namespace

int jobname_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args, {"Usage: jobname JOB_SPEC NEW_NAME", "       jobname JOB_SPEC [-c|--clear]",
                   "Assign a temporary display name to a job, or clear it."})) {
        return 0;
    }

    if (args.size() < 3) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "jobname",
                     "missing job spec or new name",
                     {"Usage: jobname JOB_SPEC NEW_NAME", "       jobname JOB_SPEC [-c|--clear]"}});
        return 1;
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();

    std::vector<std::string> resolve_args = {"jobname", args[1]};
    auto resolved = job_control_helpers::resolve_control_job_target(resolve_args, job_manager);
    if (!resolved) {
        return 1;
    }

    auto job = resolved->job;
    if (!job) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "jobname",
                     args[1] + ": no such job",
                     {"Use 'jobs' to list available jobs"}});
        return 1;
    }

    const bool clear_name = args.size() == 3 && (args[2] == "--clear" || args[2] == "-c");

    if (clear_name) {
        job->set_custom_name({});
    } else {
        std::string new_name = normalize_name(args);
        if (new_name.empty() || is_blank(new_name)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "jobname",
                         "new name cannot be empty",
                         {"Provide the desired display name after the job spec"}});
            return 1;
        }

        job->set_custom_name(new_name);
    }
    std::cout << "[" << job->job_id << "] => " << job->display_command() << '\n';
    return 0;
}

int kill_command(const std::vector<std::string>& args) {
    auto run = [&]() -> int {
        static const std::vector<std::string> usage_lines = {
            "Usage: kill [-s SIGNAL|-n SIGNUM|-SIGNAL] ID ...",
            "       kill -l",
            "Send a signal to processes or jobs.",
            "  -s SIGNAL  Specify a signal by name or number",
            "  -n SIGNUM  Specify a signal number",
            "  -SIGNAL    Specify a signal by name or number",
            "  -l          List available signals"};
        if (builtin_handle_help(args, usage_lines)) {
            return 0;
        }
        if (args.size() < 2) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "kill",
                         "no targets specified",
                         {"Usage: kill [-s SIGNAL|-n SIGNUM|-SIGNAL] ID ..."}});
            return 2;
        }

        int signal = SIGTERM;
        size_t start_index = 1;

        if (!args[1].empty() && args[1][0] == '-') {
            if (args[1] == "-l") {
                if (args.size() != 2) {
                    print_error({ErrorType::INVALID_ARGUMENT, "kill", "-l accepts no operands",
                                 usage_lines});
                    return 2;
                }
                std::cout << kill_signal_list_text() << '\n';
                return 0;
            }

            std::string signal_str;
            if (args[1] == "-s" || args[1] == "-n") {
                if (args.size() < 3) {
                    print_error({ErrorType::INVALID_ARGUMENT, "kill",
                                 args[1] + " requires a signal", usage_lines});
                    return 2;
                }
                signal_str = args[2];
                start_index = 3;
            } else {
                signal_str = args[1].substr(1);
                start_index = 2;
            }

            if (args[1] == "-n" &&
                !std::all_of(signal_str.begin(), signal_str.end(),
                             [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "kill",
                             "invalid signal number: " + signal_str,
                             {"Use -l to list valid signals"}});
                return 1;
            }

            signal = job_control_helpers::parse_signal(signal_str);
            if (signal == -1) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "kill",
                             "invalid signal: " + signal_str,
                             {"Use -l to list valid signals"}});
                return 1;
            }
        }

        if (start_index >= args.size()) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "kill",
                         "no targets specified",
                         {"Usage: kill [-s SIGNAL|-n SIGNUM|-SIGNAL] ID ..."}});
            return 2;
        }

        auto& job_manager = JobManager::instance();
        job_manager.update_job_statuses();

        auto is_stop_signal = [](int sig) {
            switch (sig) {
#ifdef SIGSTOP
                case SIGSTOP:
                    return true;
#endif
#ifdef SIGTSTP
                case SIGTSTP:
                    return true;
#endif
#ifdef SIGTTIN
                case SIGTTIN:
                    return true;
#endif
#ifdef SIGTTOU
                case SIGTTOU:
                    return true;
#endif
                default:
                    return false;
            }
        };

        auto is_continue_signal = [](int sig) {
#ifdef SIGCONT
            return sig == SIGCONT;
#else
            (void)sig;
            return false;
#endif
        };

        auto update_job_state_after_signal = [&](const std::shared_ptr<JobControlJob>& job) {
            if (!job) {
                return;
            }
            if (is_stop_signal(signal)) {
                job->state.store(JobState::STOPPED, std::memory_order_relaxed);
                job->stop_signal = signal;
            } else if (is_continue_signal(signal)) {
                job->state.store(JobState::RUNNING, std::memory_order_relaxed);
                job->stop_signal = 0;
                job->stopped_pids.clear();
                job->stop_notified.store(false, std::memory_order_relaxed);
            }
        };

        bool had_error = false;

        auto send_signal_to_job = [&](const std::shared_ptr<JobControlJob>& job) {
            if (!job) {
                return;
            }
            if (!signal_job_processes(job, signal)) {
                print_error_errno({ErrorType::RUNTIME_ERROR, "kill", "signal job", {}});
                had_error = true;
                return;
            }
            update_job_state_after_signal(job);
        };

        auto handle_job_target = [&](const std::string& original) -> bool {
            std::vector<std::string> lookup_args = {"kill", original};
            auto resolved =
                job_control_helpers::resolve_control_job_target(lookup_args, job_manager);
            if (!resolved) {
                had_error = true;
                return false;
            }

            send_signal_to_job(resolved->job);
            return true;
        };

        for (size_t i = start_index; i < args.size(); ++i) {
            const std::string& target = args[i];

            if (!target.empty() && target[0] == '%') {
                handle_job_target(target);
                continue;
            }

            bool treated_as_pid = false;
            auto parsed_pid = job_control_helpers::parse_pid_specifier(target);
            if (parsed_pid.has_value()) {
                pid_t pid = *parsed_pid;
                if (kill(pid, signal) < 0) {
                    print_error_errno({ErrorType::RUNTIME_ERROR, "kill", "kill", {}});
                    had_error = true;
                } else {
                    auto job = job_manager.get_job_by_pid_or_pgid(pid);
                    update_job_state_after_signal(job);
                }
                treated_as_pid = true;
            }

            if (!treated_as_pid) {
                handle_job_target(target);
            }
        }

        return had_error ? 1 : 0;
    };

    try {
        return run();
    } catch (...) {
        print_error({ErrorType::INVALID_ARGUMENT, "kill", "unable to process arguments", {}});
        return 1;
    }
}
