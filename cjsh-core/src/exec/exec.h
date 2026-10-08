/*
  exec.h

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

#ifndef CJSH_CORE_SRC_EXEC_EXEC_H
#define CJSH_CORE_SRC_EXEC_EXEC_H

#include <csignal>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "error_out.h"
#include "job_state.h"  // IWYU pragma: export

struct Command;
namespace cjsh_env {
struct PreparedCommand;
}

class Exec {
   private:
    std::mutex error_mutex;
    std::mutex jobs_mutex;
    std::map<int, std::shared_ptr<Job>> jobs;
    int next_job_id = 1;
    pid_t shell_pgid;
    int shell_terminal;
    bool owns_shell_terminal = false;
    bool shell_is_interactive;
    int last_exit_code = 0;
    std::vector<int> last_pipeline_statuses;
    ErrorInfo last_error;

    bool handle_empty_args(const std::vector<std::string>& args);
    std::optional<int> handle_prepared_assignments(const cjsh_env::PreparedCommand& command,
                                                   bool asynchronous);
    std::optional<int> run_command_not_found_handler(
        const std::vector<std::string>& args,
        const std::vector<std::pair<std::string, std::string>>& assignments, bool is_builtin,
        const std::string& cached_exec_path);
    bool requires_fork(const Command& cmd) const;
    bool can_execute_in_process(const Command& cmd) const;
    int execute_builtin_with_redirections(Command cmd);
    void set_error_from_wait_status(const std::string& command, int status);
    void warn_parent_setpgid_failure();

    Job* find_job_locked(int job_id);
    Job* find_job_and_set_output_forwarding_locked(int job_id, bool forward);
    void resume_job(Job& job, bool cont, std::string_view context);
    void report_missing_job(int job_id);

    void set_last_pipeline_statuses(std::vector<int> statuses);

   public:
    Exec();
    ~Exec();

    int execute_command_async(const std::vector<std::string>& args);
    int execute_prepared_command_sync(cjsh_env::PreparedCommand command,
                                      bool auto_background_on_stop = false,
                                      bool auto_background_on_stop_silent = false);
    int execute_prepared_command_async(cjsh_env::PreparedCommand command);
    int execute_pipeline(const std::vector<Command>& commands);
    int run_with_command_redirections(Command cmd, const std::function<int()>& action,
                                      const std::string& command_name, bool persist_fd_changes,
                                      bool* action_invoked = nullptr,
                                      bool preserve_action_fds = true);

    int add_job(const std::shared_ptr<Job>& job);
    void remove_job(int job_id);
    void put_job_in_foreground(int job_id, bool cont);
    void put_job_in_background(int job_id, bool cont);
    void wait_for_job(int job_id);
    std::shared_ptr<Job> find_job_by_pid(pid_t pid);
    std::map<int, std::shared_ptr<Job>> get_jobs();
    void terminate_all_child_process(int signal = SIGTERM);
    void abandon_all_child_processes();
    void remove_job_by_pgid(pid_t pgid);

    void set_error(const ErrorInfo& error);
    void set_error(ErrorType type, const std::string& command = "", const std::string& message = "",
                   const std::vector<std::string>& suggestions = {});
    ErrorInfo get_error();
    void print_last_error();
    void print_error_if_needed(int exit_code);
    const std::vector<int>& get_last_pipeline_statuses() const;
};

namespace exec_utils {

struct CommandOutput {
    std::string output;
    int exit_code;
    bool success;
    std::string error_output;
};

CommandOutput execute_with_stdout_capture(const std::function<int()>& child_executor,
                                          bool capture_stderr = false, bool suppress_stderr = true);

CommandOutput execute_command_for_output(const std::string& command);

CommandOutput execute_command_vector_for_output(const std::vector<std::string>& args);

// Capture stderr separately so diagnostics do not become part of the stdout protocol.
CommandOutput execute_command_vector_for_output_with_progress(
    const std::vector<std::string>& args, const std::function<void()>& progress_callback,
    unsigned int progress_interval_ms = 250,
    const std::function<bool()>& cancellation_callback = {});

}  // namespace exec_utils

#endif  // CJSH_CORE_SRC_EXEC_EXEC_H
