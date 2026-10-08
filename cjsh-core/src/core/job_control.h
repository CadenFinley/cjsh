/*
  job_control.h

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

#ifndef CJSH_CORE_SRC_CORE_JOB_CONTROL_H
#define CJSH_CORE_SRC_CORE_JOB_CONTROL_H

#include <sys/types.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "error_out.h"
#include "job_state.h"  // IWYU pragma: export
#include "parser.h"

class Shell;
class Exec;

class JobManager {
   public:
    static JobManager& instance();

    int add_job(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command,
                bool background = false, bool reads_stdin = true, bool process_group = true);

    int add_job(const std::shared_ptr<JobControlJob>& job, const std::string& command,
                bool reads_stdin);

    void remove_job(int job_id);

    std::shared_ptr<JobControlJob> get_job(int job_id);

    std::shared_ptr<JobControlJob> get_job_by_pid(pid_t pid);

    std::shared_ptr<JobControlJob> get_job_by_pid_or_pgid(pid_t id);

    std::vector<std::shared_ptr<JobControlJob>> get_all_jobs();

    void update_job_statuses();

    void set_current_job(int job_id);

    int get_current_job() const;

    int get_previous_job() const;

    void set_last_background_pid(pid_t pid);

    pid_t get_last_background_pid() const;

    void cleanup_finished_jobs(bool at_prompt = false);

    void set_shell(Shell* shell);

    void notify_job_stopped(const std::shared_ptr<JobControlJob>& job) const;
    void notify_job_finished(const std::shared_ptr<JobControlJob>& job) const;
    void handle_child_status(pid_t pid, int status, Exec* executor = nullptr);

    bool foreground_job_reads_stdin();

    void clear_stdin_signal(pid_t pid);

    void clear_all_jobs();
    std::optional<int> consume_completed_pid_status(pid_t pid);

   private:
    JobManager() = default;
    std::unordered_map<int, std::shared_ptr<JobControlJob>> jobs;
    int next_job_id = 1;
    int current_job = -1;
    int previous_job = -1;
    pid_t last_background_pid = -1;
    std::unordered_map<pid_t, int> completed_pid_statuses;
    Shell* shell = nullptr;
    bool allow_deferred_notifications = false;

    void update_current_previous(int new_current);
};

namespace job_control_helpers {

struct ResolvedJob {
    int job_id;
    std::shared_ptr<JobControlJob> job;
};

std::shared_ptr<JobControlJob> find_job_by_command(const std::string& spec, JobManager& job_manager,
                                                   bool& ambiguous);

std::optional<ResolvedJob> resolve_control_job_target(const std::vector<std::string>& args,
                                                      JobManager& job_manager);

std::optional<int> wait_for_job(const std::shared_ptr<JobControlJob>& job, JobManager& job_manager,
                                bool return_on_stop = true, pid_t* status_pid = nullptr);

std::optional<pid_t> parse_pid_specifier(const std::string& target);

int parse_signal(const std::string& signal_str);

}  // namespace job_control_helpers

namespace job_utils {

struct ExitErrorResult {
    ErrorType type;
    std::string message;
    std::vector<std::string> suggestions;
};

ExitErrorResult make_exit_error_result(const std::string& command, int exit_code,
                                       const std::string& success_message,
                                       const std::string& failure_prefix);

bool command_consumes_terminal_stdin(const Command& cmd);
bool pipeline_consumes_terminal_stdin(const std::vector<Command>& commands);

}  // namespace job_utils

#endif  // CJSH_CORE_SRC_CORE_JOB_CONTROL_H
