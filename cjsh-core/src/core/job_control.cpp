/*
  job_control.cpp

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

#include "job_control.h"

#include "cjsh_filesystem.h"
#include "exec.h"
#include "isocline.h"
#include "numeric_utils.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "suggestion_utils.h"
#include "wait_status_utils.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "error_out.h"

// Manage job lookup, notifications, and statuses retained for wait.
// Execution and job control update the same records.
namespace {

// let the editor place notifications around active input when possible. stderr
// remains the fallback for noninteractive use or an unavailable notification queue.
void print_job_notification(const std::string& message) {
    if (config::interactive_mode && isatty(STDERR_FILENO) &&
        ic_queue_notification(message.c_str())) {
        return;
    }
    std::cerr << '\n' << message;
}

std::string signal_status_message(int signal_number) {
    std::string signal_name = SignalHandler::signal_to_name(signal_number);
    std::string description = "Terminated by signal " + std::to_string(signal_number);

    for (const auto& signal : SignalHandler::available_signals()) {
        if (signal.signal == signal_number && signal.description != nullptr) {
            description = signal.description;
            break;
        }
    }

    const size_t context_start = description.find(" (");
    if (context_start != std::string::npos) {
        (void)description.erase(context_start);
    }
    if (!signal_name.empty() && signal_name != std::to_string(signal_number)) {
        description += " (" + signal_name + ")";
    }
    return description;
}

enum class JobMatchKind : uint8_t {
    None,
    Exact,
    Prefix
};

JobMatchKind job_command_match_kind(const std::shared_ptr<JobControlJob>& job,
                                    const std::string& spec) {
    if (spec.empty()) {
        return JobMatchKind::None;
    }

    const auto& comparison_source = job->has_custom_name() ? job->custom_name : job->command;
    const auto trimmed_command = string_utils::trim_ascii_whitespace_copy(comparison_source);
    if (trimmed_command.empty()) {
        return JobMatchKind::None;
    }

    const std::string_view spec_view(spec);
    if (trimmed_command == spec_view) {
        return JobMatchKind::Exact;
    }

    const auto first_space = trimmed_command.find_first_of(" \t");
    std::string_view command_word(trimmed_command);
    if (first_space != std::string::npos) {
        command_word = command_word.substr(0, first_space);
    }

    if (command_word == spec_view) {
        return JobMatchKind::Exact;
    }

    if (command_word.rfind(spec_view, 0) == 0) {
        return JobMatchKind::Prefix;
    }

    return JobMatchKind::None;
}

// share jobspec policy across control builtins. absent operands use the current
// job with a newest-job fallback; explicit operands must resolve unambiguously.
std::shared_ptr<JobControlJob> resolve_job_argument(const std::vector<std::string>& args,
                                                    JobManager& job_manager, int& job_id_out) {
    job_id_out = job_manager.get_current_job();

    if (args.size() <= 1) {
        auto job = job_manager.get_job(job_id_out);
        if (job) {
            return job;
        }

        const auto jobs = job_manager.get_all_jobs();
        if (jobs.empty()) {
            print_error({ErrorType::UNKNOWN_ERROR, args[0], "no current job", {}});
            return nullptr;
        }

        auto fallback = jobs.back();
        job_id_out = fallback->job_id;
        job_manager.set_current_job(job_id_out);
        return fallback;
    }

    const std::string& original_spec = args[1];
    std::string job_spec = string_utils::trim_ascii_whitespace_copy(original_spec);

    if (job_spec.empty()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     args[0],
                     original_spec + ": no such job",
                     {"Use 'jobs' to list available jobs"}});
        return nullptr;
    }

    auto resolve_relative_job = [&](char marker) -> std::shared_ptr<JobControlJob> {
        int target_id =
            marker == '+' ? job_manager.get_current_job() : job_manager.get_previous_job();
        if (target_id < 0) {
            print_error(
                {ErrorType::INVALID_ARGUMENT,
                 args[0],
                 original_spec + (marker == '+' ? ": current job not set" : ": no previous job"),
                 {"Use 'jobs' to list available jobs"}});
            return nullptr;
        }

        auto job = job_manager.get_job(target_id);
        if (!job) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         args[0],
                         original_spec + ": no such job",
                         {"Use 'jobs' to list available jobs"}});
            return nullptr;
        }

        job_id_out = job->job_id;
        return job;
    };

    const bool explicit_jobspec = job_spec[0] == '%';
    if (explicit_jobspec) {
        (void)job_spec.erase(0, 1);
    }

    if ((explicit_jobspec && (job_spec.empty() || job_spec == "%" || job_spec == "+")) ||
        (!explicit_jobspec && job_spec == "+")) {
        return resolve_relative_job('+');
    }
    if (job_spec == "-") {
        return resolve_relative_job('-');
    }

    int parsed_value = 0;
    if (numeric_utils::parse_int_strict(job_spec, parsed_value) && parsed_value > 0) {
        auto job = job_manager.get_job(parsed_value);
        if (job) {
            job_id_out = parsed_value;
            return job;
        }

        // a leading '%' makes a number unambiguously a job id. bare numbers may
        // also identify a process or process-group leader for control builtins.
        if (!explicit_jobspec) {
            auto job_by_pid = job_manager.get_job_by_pid_or_pgid(static_cast<pid_t>(parsed_value));
            if (job_by_pid) {
                job_id_out = job_by_pid->job_id;
                return job_by_pid;
            }
        }
    }

    if (!job_spec.empty() && job_spec[0] == '?') {
        const std::string needle = job_spec.substr(1);
        std::shared_ptr<JobControlJob> match;
        if (!needle.empty()) {
            for (const auto& candidate : job_manager.get_all_jobs()) {
                if (candidate->display_command().find(needle) == std::string::npos) {
                    continue;
                }
                if (match) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 args[0],
                                 original_spec + ": multiple jobs match command",
                                 {"Use job id or PID to disambiguate"}});
                    return nullptr;
                }
                match = candidate;
            }
        }
        if (match) {
            job_id_out = match->job_id;
            return match;
        }
    }

    bool ambiguous = false;
    auto job = job_control_helpers::find_job_by_command(job_spec, job_manager, ambiguous);
    if (job) {
        job_id_out = job->job_id;
        return job;
    }

    if (ambiguous) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     args[0],
                     original_spec + ": multiple jobs match command",
                     {"Use job id or PID to disambiguate"}});
    } else {
        print_error({ErrorType::INVALID_ARGUMENT,
                     args[0],
                     original_spec + ": no such job",
                     {"Use 'jobs' to list available jobs"}});
    }

    return nullptr;
}

}  // namespace

namespace job_control_helpers {

int parse_signal(const std::string& signal_str) {
    if (signal_str.empty()) {
        return SIGTERM;
    }

    int resolved = SignalHandler::name_to_signal(signal_str);
    if (resolved == 0) {
        // POSIX allows signal 0 as a special case for error checking.
        return 0;
    }

    if (resolved > 0 && SignalHandler::is_valid_signal(resolved)) {
        return resolved;
    }

    return -1;
}

// prefer an exact command/custom-name match over prefixes. multiple exact matches
// or multiple prefixes without an exact match require an explicit id from the user.
std::shared_ptr<JobControlJob> find_job_by_command(const std::string& spec, JobManager& job_manager,
                                                   bool& ambiguous) {
    ambiguous = false;
    std::shared_ptr<JobControlJob> exact_match;
    std::vector<std::shared_ptr<JobControlJob>> prefix_matches;

    const auto jobs = job_manager.get_all_jobs();
    for (const auto& job : jobs) {
        switch (job_command_match_kind(job, spec)) {
            case JobMatchKind::Exact:
                if (exact_match) {
                    ambiguous = true;
                    return nullptr;
                }
                exact_match = job;
                break;
            case JobMatchKind::Prefix:
                prefix_matches.push_back(job);
                break;
            case JobMatchKind::None:
                break;
        }
    }

    if (exact_match) {
        return exact_match;
    }

    if (prefix_matches.size() == 1) {
        return prefix_matches.front();
    }

    if (!prefix_matches.empty()) {
        ambiguous = true;
    }

    return nullptr;
}

std::optional<ResolvedJob> resolve_control_job_target(const std::vector<std::string>& args,
                                                      JobManager& job_manager) {
    int job_id = job_manager.get_current_job();
    auto job = resolve_job_argument(args, job_manager, job_id);
    if (!job) {
        return std::nullopt;
    }
    return ResolvedJob{job->job_id, job};
}

// wait using aggregate job state rather than assuming the last waitpid result is
// the pipeline result. stop policy belongs to the caller, and terminal ownership
// must already have been arranged by a foreground-control caller when needed.
std::optional<int> wait_for_job(const std::shared_ptr<JobControlJob>& job, JobManager& job_manager,
                                bool return_on_stop, pid_t* status_pid) {
    if (!job) {
        return std::nullopt;
    }

    auto current_result = [&]() -> std::optional<int> {
        const JobState state = job->state.load(std::memory_order_relaxed);
        if (state == JobState::DONE || state == JobState::TERMINATED) {
            if (status_pid != nullptr) {
                *status_pid = job->last_pid;
            }
            return job->exit_status;
        }
        if (return_on_stop && state == JobState::STOPPED) {
            return 128 + (job->stop_signal > 0 ? job->stop_signal : SIGTSTP);
        }
        return std::nullopt;
    };

    if (auto ready = current_result()) {
        return ready;
    }

    for (;;) {
        if (job->remaining_pids.empty()) {
            return current_result();
        }

        // without monitor-mode grouping, wait for tracked pids individually rather
        // than accidentally waiting for the shell's own process group.
        const pid_t target = job->process_group ? -job->pgid : *job->remaining_pids.begin();
        int status = 0;
        const pid_t pid = waitpid(target, &status, WUNTRACED | WCONTINUED);
        if (pid < 0) {
            if (errno == EINTR) {
                if (shell) {
                    (void)shell->process_pending_signals();
                }
                if (cjsh_env::exit_requested()) {
                    return std::nullopt;
                }
                if (auto ready = current_result()) {
                    return ready;
                }
                continue;
            }
            if (errno == ECHILD) {
                // another safe-point reaper may already have published completion.
                // consult the shared job state before treating this as unavailable.
                job_manager.update_job_statuses();
                if (auto ready = current_result()) {
                    return ready;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        // Apply each wait report once to the shared job record.
        job_manager.handle_child_status(pid, status);
        if (status_pid != nullptr && (WIFEXITED(status) || WIFSIGNALED(status))) {
            *status_pid = pid;
        }
        if (auto ready = current_result()) {
            return ready;
        }
    }
}

std::optional<pid_t> parse_pid_specifier(const std::string& target) {
    int parsed_value = 0;
    if (!numeric_utils::parse_int_strict(target, parsed_value)) {
        return std::nullopt;
    }
    return static_cast<pid_t>(parsed_value);
}

}  // namespace job_control_helpers

namespace job_utils {

ExitErrorResult make_exit_error_result(const std::string& command, int exit_code,
                                       const std::string& success_message,
                                       const std::string& failure_prefix) {
    ExitErrorResult result{ErrorType::RUNTIME_ERROR, success_message, {}};
    if (exit_code == 0) {
        return result;
    }
    result.message = failure_prefix + std::to_string(exit_code);
    if (exit_code == 127) {
        if (!cjsh_filesystem::command_exists(command)) {
            result.type = ErrorType::COMMAND_NOT_FOUND;
            result.message.clear();
            result.suggestions = suggestion_utils::generate_command_suggestions_if_enabled(command);
            return result;
        }
    } else if (exit_code == 126) {
        result.type = ErrorType::PERMISSION_DENIED;
    }
    return result;
}

// classify whether a command can still inherit terminal input. this is redirection
// metadata, not evidence that the process is currently blocked in a read.
bool command_consumes_terminal_stdin(const Command& cmd) {
    return !cmd.redirects_fd(STDIN_FILENO);
}

// only a foreground pipeline's first stage inherits the shell's stdin; later
// stages receive pipe input and cannot establish terminal-read eligibility here.
bool pipeline_consumes_terminal_stdin(const std::vector<Command>& commands) {
    if (commands.empty()) {
        return false;
    }

    if (commands.back().background) {
        return false;
    }

    return command_consumes_terminal_stdin(commands.front());
}

}  // namespace job_utils

// retain launch order and the last pipeline pid even after children exit. the
// remaining and stopped sets track aggregate lifecycle independently of that order.
JobControlJob::JobControlJob(int id, pid_t group_id, const std::vector<pid_t>& process_ids,
                             const std::string& cmd, bool is_background, bool consumes_stdin,
                             bool has_process_group)
    : job_id(id),
      pgid(group_id),
      pids(process_ids),
      remaining_pids(process_ids.begin(), process_ids.end()),
      last_pid(process_ids.empty() ? -1 : process_ids.back()),
      command(cmd),
      command_name(cmd),
      pipeline_statuses(process_ids.size(), -1),
      background(is_background),
      process_group(has_process_group),
      reads_stdin(consumes_stdin) {
}

void JobControlJob::mark_running() {
    state.store(JobState::RUNNING, std::memory_order_relaxed);
    stopped_pids.clear();
    stop_signal = 0;
    stop_notified.store(false, std::memory_order_relaxed);
}

void JobControlJob::record_wait_status(pid_t pid, int wait_status) {
    const auto position = std::find(pids.begin(), pids.end(), pid);
    if (position == pids.end()) {
        return;
    }
    if (WIFSTOPPED(wait_status)) {
        stopped_pids.insert(pid);
        stop_signal = WSTOPSIG(wait_status);
        status = wait_status;
    } else if (WIFCONTINUED(wait_status)) {
        stopped_pids.erase(pid);
        stop_signal = 0;
        state.store(JobState::RUNNING, std::memory_order_relaxed);
        stop_notified.store(false, std::memory_order_relaxed);
        return;
    } else if (WIFEXITED(wait_status) || WIFSIGNALED(wait_status)) {
        status = wait_status;
        const size_t index = static_cast<size_t>(std::distance(pids.begin(), position));
        pipeline_statuses[index] = wait_status_utils::to_exit_code(wait_status);
        if (pid == last_pid || last_pid <= 0) {
            last_status = wait_status;
            termination_signal = WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
            exit_status = wait_status_utils::to_exit_code(wait_status);
        }
        stopped_pids.erase(pid);
        remaining_pids.erase(pid);
    } else {
        return;
    }

    if (remaining_pids.empty()) {
        state.store(termination_signal == 0 ? JobState::DONE : JobState::TERMINATED,
                    std::memory_order_relaxed);
    } else if (stopped_pids.size() >= remaining_pids.size()) {
        state.store(JobState::STOPPED, std::memory_order_relaxed);
    } else {
        state.store(JobState::RUNNING, std::memory_order_relaxed);
    }
}

JobManager& JobManager::instance() {
    static JobManager instance;
    return instance;
}

int JobManager::add_job(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command,
                        bool background, bool reads_stdin, bool process_group) {
    return add_job(std::make_shared<JobControlJob>(0, pgid, pids, command, background, reads_stdin,
                                                   process_group),
                   command, reads_stdin);
}

int JobManager::add_job(const std::shared_ptr<JobControlJob>& job, const std::string& command,
                        bool reads_stdin) {
    const int job_id = next_job_id++;
    job->job_id = job_id;
    job->command = command;
    job->reads_stdin = reads_stdin;
    jobs[job_id] = job;
    update_current_previous(job_id);
    return job_id;
}

void JobManager::remove_job(int job_id) {
    auto it = jobs.find(job_id);
    if (it != jobs.end()) {
        (void)jobs.erase(it);

        if (current_job == job_id) {
            current_job = previous_job;
        }
        if (previous_job == job_id || previous_job == current_job) {
            previous_job = -1;
        }

        // keep %+ and %- useful after completion or disown. use the newest remaining
        // ids when recency markers no longer name entries in the table.
        if (current_job < 0 || jobs.find(current_job) == jobs.end()) {
            current_job = -1;
            for (const auto& [id, job] : jobs) {
                (void)job;
                current_job = std::max(current_job, id);
            }
        }
        if (previous_job < 0 || jobs.find(previous_job) == jobs.end()) {
            previous_job = -1;
            for (const auto& [id, job] : jobs) {
                (void)job;
                if (id != current_job) {
                    previous_job = std::max(previous_job, id);
                }
            }
        }
    }
}

std::shared_ptr<JobControlJob> JobManager::get_job(int job_id) {
    auto it = jobs.find(job_id);
    return it != jobs.end() ? it->second : nullptr;
}

std::shared_ptr<JobControlJob> JobManager::get_job_by_pid(pid_t pid) {
    for (const auto& pair : jobs) {
        const auto& job = pair.second;
        if (std::find(job->pids.begin(), job->pids.end(), pid) != job->pids.end()) {
            return job;
        }
    }
    return nullptr;
}

std::shared_ptr<JobControlJob> JobManager::get_job_by_pid_or_pgid(pid_t id) {
    for (const auto& pair : jobs) {
        const auto& job = pair.second;
        const bool matches_process =
            job->pgid == id || std::find(job->pids.begin(), job->pids.end(), id) != job->pids.end();
        if (matches_process) {
            return job;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<JobControlJob>> JobManager::get_all_jobs() {
    std::vector<std::shared_ptr<JobControlJob>> result;
    result.reserve(jobs.size());
    for (const auto& pair : jobs) {
        result.push_back(pair.second);
    }

    std::sort(result.begin(), result.end(),
              [](const std::shared_ptr<JobControlJob>& a, const std::shared_ptr<JobControlJob>& b) {
                  return a->job_id < b->job_id;
              });

    return result;
}

// poll only registered live children, leaving captured prompt workers to their
// own waiters. gather reports before applying state changes and notifications.
void JobManager::update_job_statuses() {
    std::vector<std::pair<pid_t, int>> status_changes;

    // poll a snapshot because handle_child_status mutates each job's live-process set.
    for (auto& pair : jobs) {
        auto job = pair.second;
        const std::vector<pid_t> pids(job->remaining_pids.begin(), job->remaining_pids.end());

        for (pid_t pid : pids) {
            int status = 0;
            int status_updates = 0;
            const int max_status_updates = 16;

            while (status_updates < max_status_updates) {
                pid_t result = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

                if (result == 0) {
                    break;
                }

                if (result == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == ECHILD) {
                        break;
                    }
                    print_error({ErrorType::RUNTIME_ERROR,
                                 ErrorSeverity::WARNING,
                                 "waitpid",
                                 "failed to poll pid " + std::to_string(pid) + ": " +
                                     std::string(std::strerror(errno)),
                                 {}});
                    break;
                }

                ++status_updates;
                (void)status_changes.emplace_back(pid, status);
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    break;
                }
            }
        }
    }

    for (const auto& [pid, status] : status_changes) {
        handle_child_status(pid, status);
    }
}

void JobManager::set_current_job(int job_id) {
    update_current_previous(job_id);
}

int JobManager::get_current_job() const {
    return current_job;
}

int JobManager::get_previous_job() const {
    return previous_job;
}

void JobManager::set_last_background_pid(pid_t pid) {
    last_background_pid = pid;
}

pid_t JobManager::get_last_background_pid() const {
    return last_background_pid;
}

void JobManager::set_shell(Shell* shell) {
    this->shell = shell;
}

// emit a stop once per stop/resume cycle. POSIX background notifications can wait
// for a prompt boundary unless notify mode requests immediate reporting.
void JobManager::notify_job_stopped(const std::shared_ptr<JobControlJob>& job) const {
    if (!job || job->stop_notified.load(std::memory_order_relaxed)) {
        return;
    }

    if (!config::interactive_mode && !config::force_interactive) {
        return;
    }

    if (config::is_posix_mode() && job->background.load(std::memory_order_relaxed) &&
        !allow_deferred_notifications && shell && !shell->get_shell_option(ShellOption::Notify)) {
        return;
    }

    job->state.store(JobState::STOPPED, std::memory_order_relaxed);

    char status_char = ' ';
    if (job->job_id == current_job) {
        status_char = '+';
    } else if (job->job_id == previous_job) {
        status_char = '-';
    }

    std::ostringstream message;
    message << '[' << job->job_id << "]" << status_char << "  Stopped\t" << job->display_command()
            << '\n';
    print_job_notification(message.str());

    job->stop_notified.store(true, std::memory_order_relaxed);
}

// notification completion controls when cleanup may remove a finished job.
// deliberately silent cases still mark it notified; deferred cases leave it pending.
void JobManager::notify_job_finished(const std::shared_ptr<JobControlJob>& job) const {
    if (!job || job->notified) {
        return;
    }
    if (job->suppress_notifications) {
        job->notified = true;
        return;
    }

    const JobState state = job->state.load(std::memory_order_relaxed);
    const bool is_background = job->background.load(std::memory_order_relaxed);
    const bool is_interactive = config::interactive_mode || config::force_interactive;

    if (config::is_posix_mode() && !is_interactive) {
        job->notified = true;
        return;
    }

    if (state == JobState::DONE && !is_background) {
        job->notified = true;
        return;
    }
    if (state == JobState::TERMINATED && !is_background && !is_interactive) {
        job->notified = true;
        return;
    }
    if (state != JobState::DONE && state != JobState::TERMINATED) {
        return;
    }
    if (config::is_posix_mode() && is_background && !allow_deferred_notifications && shell &&
        !shell->get_shell_option(ShellOption::Notify)) {
        return;
    }

    std::ostringstream message;
    message << '[' << job->job_id << "]";
    if (!is_background) {
        char status_char = ' ';
        if (job->job_id == current_job) {
            status_char = '+';
        } else if (job->job_id == previous_job) {
            status_char = '-';
        }
        message << status_char << "  ";
    } else {
        message << ' ';
    }

    if (state == JobState::TERMINATED) {
        message << signal_status_message(job->termination_signal);
    } else if (job->exit_status == 0) {
        message << "Done";
    } else {
        message << "Exit " << job->exit_status;
    }
    message << '\t' << job->display_command() << '\n';
    print_job_notification(message.str());
    job->notified = true;
}

// apply a waitpid report, not a signal-handler callback. retain a bounded cache of
// terminal statuses even for pids no longer visible, for later wait calls to consume.
void JobManager::handle_child_status(pid_t pid, int status, Exec* executor) {
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        if (completed_pid_statuses.size() >= 256) {
            completed_pid_statuses.erase(completed_pid_statuses.begin());
        }
        completed_pid_statuses[pid] = wait_status_utils::to_exit_code(status);
    }

    auto job = get_job_by_pid(pid);
    const bool managed = job != nullptr;
    if (!job) {
        if (executor == nullptr && shell) {
            executor = shell->executor.get();
        }
        if (executor != nullptr) {
            job = executor->find_job_by_pid(pid);
        }
    }
    if (!job) {
        return;
    }

    job->record_wait_status(pid, status);
    if (managed) {
        if (WIFCONTINUED(status) || WIFEXITED(status) || WIFSIGNALED(status)) {
            clear_stdin_signal(job->pgid);
        }
        if (job->completed()) {
            notify_job_finished(job);
        } else if (job->stopped() && !job->defer_stop_notification) {
            notify_job_stopped(job);
        }
    }
}

void JobManager::update_current_previous(int new_current) {
    if (current_job != new_current) {
        previous_job = current_job;
        current_job = new_current;
    }
}

// prompt boundaries release deferred notifications. collect removals separately
// so notification and recency updates cannot invalidate this traversal; retained
// per-pid completion statuses are independent of visible job records.
void JobManager::cleanup_finished_jobs(bool at_prompt) {
    allow_deferred_notifications = at_prompt;
    std::vector<int> to_remove;

    for (const auto& pair : jobs) {
        auto job = pair.second;
        const JobState state = job->state.load(std::memory_order_relaxed);
        if (at_prompt && state == JobState::STOPPED) {
            notify_job_stopped(job);
        }
        if (state == JobState::DONE || state == JobState::TERMINATED) {
            notify_job_finished(job);

            if (job->notified) {
                to_remove.push_back(job->job_id);
            }
        }
    }

    for (int job_id : to_remove) {
        remove_job(job_id);
    }
    allow_deferred_notifications = false;
}

// gate editor typeahead using the current foreground job's stdin-signal state.
// eligibility alone is insufficient; retain a short grace period after a signal
// so the editor does not immediately consume input intended for that job.
bool JobManager::foreground_job_reads_stdin() {
    if (jobs.empty()) {
        return false;
    }

    int foreground_id = current_job;
    if (foreground_id == -1) {
        return false;
    }

    auto it = jobs.find(foreground_id);
    if (it == jobs.end()) {
        return false;
    }

    const auto& job = it->second;
    if (job->background.load(std::memory_order_relaxed) || !job->reads_stdin) {
        return false;
    }

    if (job->awaiting_stdin_signal) {
        return true;
    }

    if (job->stdin_signal_count > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - job->last_stdin_signal_time;
        if (elapsed <= std::chrono::milliseconds(250)) {
            return true;
        }
    }
    return false;
}

void JobManager::clear_stdin_signal(pid_t pid) {
    auto job = get_job_by_pid_or_pgid(pid);
    if (!job) {
        return;
    }

    if (job->awaiting_stdin_signal || job->stdin_signal_count > 0) {
        job->awaiting_stdin_signal = false;
        job->last_stdin_signal = 0;
        job->stdin_signal_count = 0;
        job->last_stdin_signal_time = std::chrono::steady_clock::time_point::min();
    }
}

void JobManager::clear_all_jobs() {
    jobs.clear();
    completed_pid_statuses.clear();
    current_job = -1;
    previous_job = -1;
    last_background_pid = -1;
}

// explicit wait consumes a retained result once, unlike the non-consuming query.
// these entries can outlive the notification and removal of the visible job.
std::optional<int> JobManager::consume_completed_pid_status(pid_t pid) {
    auto it = completed_pid_statuses.find(pid);
    if (it == completed_pid_statuses.end()) {
        return std::nullopt;
    }
    int status = it->second;
    completed_pid_statuses.erase(it);
    return status;
}
