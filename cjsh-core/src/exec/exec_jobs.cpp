/*
  exec_jobs.cpp

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

#include <utility>
#include "error_out.h"
#include "exec.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "job_control.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "wait_status_utils.h"

// Execution and job control share each job record. Their indexes have separate
// lifetimes because private foreground jobs do not need a visible job number.
namespace {

int extract_exit_code(int status) {
    return wait_status_utils::to_exit_code(status, 1);
}

}  // namespace

void Exec::set_error_from_wait_status(const std::string& command, int status) {
    const int exit_code = extract_exit_code(status);
    auto exit_result = job_utils::make_exit_error_result(
        command, exit_code, "command completed successfully", "command failed with exit code ");
    set_error(exit_result.type, command, exit_result.message, exit_result.suggestions);
}

// callers hold jobs_mutex while using the returned pointer. reacquire by id after
// an unlocked wait rather than retaining a pointer across possible table mutation.
Job* Exec::find_job_locked(int job_id) {
    auto it = jobs.find(job_id);
    if (it == jobs.end()) {
        report_missing_job(job_id);
        return nullptr;
    }
    return it->second.get();
}

Job* Exec::find_job_and_set_output_forwarding_locked(int job_id, bool forward) {
    Job* job = find_job_locked(job_id);
    if (job == nullptr) {
        return nullptr;
    }

    if (job->output_relay) {
        job->output_relay->forward.store(forward);
    }

    return job;
}

void Exec::report_missing_job(int job_id) {
    set_error(ErrorType::RUNTIME_ERROR, "job", "job [" + std::to_string(job_id) + "] not found");
    print_last_error();
}

// resume only a requested stopped job. use one group signal where available;
// ungrouped jobs require per-pid delivery and tolerate children that already exited.
void Exec::resume_job(Job& job, bool cont, std::string_view context) {
    if (!cont || !job.stopped()) {
        return;
    }

    bool resumed = false;
    if (job.process_group && job.pgid > 0) {
        resumed = kill(-job.pgid, SIGCONT) == 0;
    } else {
        resumed = true;
        for (pid_t pid : job.remaining_pids) {
            if (pid > 0 && kill(pid, SIGCONT) < 0 && errno != ESRCH) {
                resumed = false;
            }
        }
    }
    if (!resumed) {
        set_error(ErrorType::RUNTIME_ERROR, "kill",
                  "failed to send SIGCONT to " + std::string(context) + ": " +
                      std::string(strerror(errno)));
    }

    job.mark_running();
}

int Exec::add_job(const std::shared_ptr<Job>& job) {
    std::lock_guard<std::mutex> lock(jobs_mutex);

    int job_id = next_job_id++;
    jobs[job_id] = job;

    return job_id;
}

void Exec::remove_job(int job_id) {
    std::lock_guard<std::mutex> lock(jobs_mutex);

    auto it = jobs.find(job_id);
    if (it != jobs.end()) {
        (void)jobs.erase(it);
    }
}

// arrange output and terminal ownership before allowing foreground work to run,
// then wait without holding the job-table mutex. stopped jobs keep terminal modes
// for a later fg; attempt to restore shell ownership before returning to the caller.
void Exec::put_job_in_foreground(int job_id, bool cont) {
    std::unique_lock<std::mutex> lock(jobs_mutex);

    Job* job = find_job_and_set_output_forwarding_locked(job_id, true);
    if (job == nullptr) {
        return;
    }

    // copied shell state in a forked child is not authority to take the tty.
    // require the original interactive shell and a job with its own process group.
    const bool main_shell_controls_terminal =
        job->process_group && shell && shell->is_job_control_enabled() &&
        shell->manages_terminal() && shell_is_interactive && (isatty(shell_terminal) != 0) &&
        shell_pgid > 0 && getpid() == shell_pgid && getpgrp() == shell_pgid;

    bool terminal_control_acquired = false;
    struct termios shell_modes{};
    const bool shell_modes_saved =
        main_shell_controls_terminal && tcgetattr(shell_terminal, &shell_modes) == 0;
    if (main_shell_controls_terminal) {
        if (tcsetpgrp(shell_terminal, job->pgid) == 0) {
            terminal_control_acquired = true;
        } else {
            if (errno != ENOTTY && errno != EINVAL && errno != EPERM) {
                set_error(ErrorType::RUNTIME_ERROR, "tcsetpgrp",
                          "warning: failed to set terminal control to job: " +
                              std::string(strerror(errno)));
            }
        }
    }

    // restore a stopped program's terminal settings before sending SIGCONT, not
    // afterward when it could already be reading or rewriting terminal state.
    if (terminal_control_acquired && cont && job->tmodes_saved) {
        (void)tcsetattr(shell_terminal, TCSADRAIN, &job->tmodes);
    }

    // children launched behind a barrier must not touch the tty before handoff.
    // release one byte per member, then close our writer so none wait indefinitely.
    if (job->launch_barrier_fd >= 0) {
        const std::string ready(job->remaining_pids.size(), 'x');
        size_t written = 0;
        while (written < ready.size()) {
            const ssize_t count =
                write(job->launch_barrier_fd, ready.data() + written, ready.size() - written);
            if (count > 0) {
                written += static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        (void)close(job->launch_barrier_fd);
        job->launch_barrier_fd = -1;
    }

    resume_job(*job, cont, "job");

    lock.unlock();

    wait_for_job(job_id);

    lock.lock();

    if (terminal_control_acquired && main_shell_controls_terminal) {
        auto current = jobs.find(job_id);
        if (current != jobs.end() && current->second->stopped() &&
            tcgetattr(shell_terminal, &current->second->tmodes) == 0) {
            current->second->tmodes_saved = true;
        }
        if (tcsetpgrp(shell_terminal, shell_pgid) < 0) {
            if (errno != ENOTTY && errno != EINVAL) {
                set_error(
                    ErrorType::RUNTIME_ERROR, "tcsetpgrp",
                    "warning: failed to restore terminal control: " + std::string(strerror(errno)));
            }
        }

        // recover pre-command modes after a stop or signal death. a normal exit
        // may intentionally leave changed settings, as with an stty invocation.
        const bool restore_modes = current != jobs.end() && (current->second->stopped() ||
                                                             WIFSIGNALED(current->second->status));
        if (restore_modes && shell_modes_saved &&
            tcsetattr(shell_terminal, TCSADRAIN, &shell_modes) < 0) {
            set_error(ErrorType::RUNTIME_ERROR, "tcsetattr",
                      "failed to restore terminal attributes: " + std::string(strerror(errno)));
        }
    }
}

// background continuation does not transfer the terminal. silence an available
// relay before resuming so its output policy matches the selected background mode.
void Exec::put_job_in_background(int job_id, bool cont) {
    std::lock_guard<std::mutex> lock(jobs_mutex);

    Job* job = find_job_and_set_output_forwarding_locked(job_id, false);
    if (job == nullptr) {
        return;
    }

    resume_job(*job, cont, "background job");
}

// Wait reports update the same record that jobs, fg, bg, and wait inspect.
void Exec::wait_for_job(int job_id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        const auto it = jobs.find(job_id);
        if (it == jobs.end()) {
            return;
        }
        job = it->second;
    }

    while (!job->remaining_pids.empty()) {
        // This waiter owns these reports. Process traps without a second child reaper.
        if (shell) {
            (void)shell->process_pending_signals(false);
        } else if (auto* handler = SignalHandler::instance()) {
            (void)handler->process_pending_signals(this, false);
        }
        if (cjsh_env::exit_requested()) {
            last_exit_code = SignalHandler::termination_signal() != 0
                                 ? 128 + SignalHandler::termination_signal()
                                 : 0;
            return;
        }
        if (job->remaining_pids.empty()) {
            break;
        }
        const auto first_live = std::find_if(job->pids.begin(), job->pids.end(), [&](pid_t pid) {
            return job->remaining_pids.count(pid) != 0;
        });
        const pid_t target = job->process_group ? -job->pgid : *first_live;
        int status = 0;
        const pid_t pid = waitpid(target, &status, WUNTRACED | WCONTINUED);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                job->remaining_pids.clear();
                job->state.store(
                    job->termination_signal == 0 ? JobState::DONE : JobState::TERMINATED,
                    std::memory_order_relaxed);
                job->status = job->last_status;
            } else {
                set_error(ErrorType::RUNTIME_ERROR, "waitpid",
                          "failed to wait for child process: " + std::string(strerror(errno)));
            }
            break;
        }
        if (job->remaining_pids.count(pid) != 0 && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGINT) {
            SignalHandler::note_startup_interrupt();
        }
        JobManager::instance().handle_child_status(pid, status, this);
        if (job->stopped()) {
            break;
        }
    }

    if (job->stopped()) {
        const bool auto_background =
            job->auto_background_on_stop && job->stop_signal == SIGTSTP && job->pgid > 0;
        if (auto_background) {
            if (job->auto_background_on_stop_silent && job->output_relay) {
                job->output_relay->forward.store(false);
            }
            resume_job(*job, true, "background job");
            job->background.store(true, std::memory_order_relaxed);
            job->defer_stop_notification = false;
            last_exit_code = 0;
            JobManager::instance().set_last_background_pid(job->last_pid);
            std::cerr << "\n[" << job_id << "]+ " << job->display_command() << " &" << '\n';
        } else {
            last_exit_code = 128 + SIGTSTP;
            job->defer_stop_notification = false;
            if (job->job_id != 0) {
                JobManager::instance().notify_job_stopped(job);
            }
        }
    } else if (job->completed()) {
        last_exit_code = extract_exit_code(job->last_status);
        set_error_from_wait_status(job->command_name, job->last_status);
    }
}

std::shared_ptr<Job> Exec::find_job_by_pid(pid_t pid) {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (const auto& [id, job] : jobs) {
        (void)id;
        if (job->remaining_pids.count(pid) != 0) {
            return job;
        }
    }
    return nullptr;
}

// Copy the index so callers can inspect shared records without the index lock.
std::map<int, std::shared_ptr<Job>> Exec::get_jobs() {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    return jobs;
}

// signal a snapshot during shell teardown, then give children a bounded chance
// to exit. disown-style hangup protection applies to SIGHUP, not all shutdown causes.
void Exec::terminate_all_child_process(int signal) {
    std::vector<std::shared_ptr<Job>> job_snapshot;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex);
        job_snapshot.reserve(jobs.size());
        for (const auto& pair : jobs) {
            job_snapshot.push_back(pair.second);
        }
    }

    const auto send_signal_to_job = [](const Job& job, int signum) {
        // signal the group once so handlers are not invoked twice for group leaders.
        if (job.process_group && job.pgid > 0 && killpg(job.pgid, signum) == 0) {
            return;
        }
        for (pid_t pid : job.remaining_pids) {
            if (pid > 0) {
                (void)kill(pid, signum);
            }
        }
    };

    std::vector<pid_t> pending_children;
    for (const auto& record : job_snapshot) {
        const Job& job = *record;
        if (job.completed() || (signal == SIGHUP && job.hup_protected)) {
            continue;
        }
        send_signal_to_job(job, signal);
        pending_children.insert(pending_children.end(), job.remaining_pids.begin(),
                                job.remaining_pids.end());
#ifdef SIGCONT
        // queue the shutdown signal before resuming a stopped job.
        if (job.stopped()) {
            send_signal_to_job(job, SIGCONT);
        }
#endif
    }

    // give ordinary children a bounded opportunity to exit and be reaped. signal
    // handlers and ignored dispositions are respected even after the grace period.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!pending_children.empty()) {
        pending_children.erase(
            std::remove_if(pending_children.begin(), pending_children.end(),
                           [](pid_t pid) {
                               int status = 0;
                               const pid_t waited = waitpid(pid, &status, WNOHANG);
                               return waited > 0 || (waited < 0 && errno == ECHILD);
                           }),
            pending_children.end());
        if (pending_children.empty() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    abandon_all_child_processes();
}

void Exec::remove_job_by_pgid(pid_t pgid) {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        if (it->second->pgid == pgid) {
            jobs.erase(it);
            return;
        }
    }
}

// discard execution records without signaling children. cleanup must not deadlock
// if reached while the table is locked, so a failed try_lock leaves records alone.
void Exec::abandon_all_child_processes() {
    if (!jobs_mutex.try_lock()) {
        return;
    }
    jobs.clear();
    jobs_mutex.unlock();
}
