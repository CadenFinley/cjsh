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
#include <iterator>
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

// manage execution-side job records, foreground waits, output relays, and child
// cleanup. JobManager supplies jobspecs and user-facing lifecycle state; waiters
// here publish reports there rather than treating the two tables as interchangeable.
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
    return &it->second;
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
    if (!cont || !job.stopped) {
        return;
    }

    bool resumed = false;
    if (job.process_group && job.pgid > 0) {
        resumed = kill(-job.pgid, SIGCONT) == 0;
    } else {
        resumed = true;
        for (pid_t pid : job.pids) {
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

    job.stopped = false;
}

int Exec::add_job(const Job& job) {
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
    bool stopped_modes_saved = false;
    pid_t stopped_job_pgid = -1;
    struct termios stopped_job_modes{};
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
        const std::string ready(job->pids.size(), 'x');
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
        if (current != jobs.end() && current->second.stopped &&
            tcgetattr(shell_terminal, &current->second.tmodes) == 0) {
            current->second.tmodes_saved = true;
            stopped_modes_saved = true;
            stopped_job_pgid = current->second.pgid;
            stopped_job_modes = current->second.tmodes;
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
        const bool restore_modes = current != jobs.end() &&
                                   (current->second.stopped || WIFSIGNALED(current->second.status));
        if (restore_modes && shell_modes_saved &&
            tcsetattr(shell_terminal, TCSADRAIN, &shell_modes) < 0) {
            set_error(ErrorType::RUNTIME_ERROR, "tcsetattr",
                      "failed to restore terminal attributes: " + std::string(strerror(errno)));
        }
    }

    // make stopped terminal modes available to the user-facing fg path too.
    // release the execution-table lock before updating the other subsystem.
    lock.unlock();
    if (stopped_modes_saved) {
        if (auto managed_job = JobManager::instance().get_job_by_pid_or_pgid(stopped_job_pgid)) {
            managed_job->tmodes = stopped_job_modes;
            managed_job->tmodes_saved = true;
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

void Exec::set_job_output_forwarding(pid_t pgid, bool forward) {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (auto& pair : jobs) {
        Job& job = pair.second;
        if (job.pgid == pgid) {
            if (job.output_relay) {
                job.output_relay->forward.store(forward);
            }
            break;
        }
    }
}

// own foreground wait reports until the job completes, fully stops, or shell exit
// interrupts the wait. preserve launch order for PIPESTATUS and the last member's
// status separately from whichever child happens to report last.
void Exec::wait_for_job(int job_id) {
    std::unique_lock<std::mutex> lock(jobs_mutex);

    auto it = jobs.find(job_id);
    if (it == jobs.end()) {
        return;
    }

    // snapshot wait inputs before releasing the mutex. signal processing and
    // job-management callbacks must remain able to access the execution table.
    pid_t job_pgid = it->second.pgid;
    bool process_group = it->second.process_group;
    std::vector<pid_t> remaining_pids = it->second.pids;
    pid_t last_pid = it->second.last_pid;
    std::vector<pid_t> pid_order = it->second.pid_order;
    std::vector<int> pipeline_statuses = it->second.pipeline_statuses;

    lock.unlock();

    int status = 0;
    pid_t pid = 0;

    bool job_stopped = false;
    bool saw_last = false;
    int last_status = 0;
    int stop_signal = 0;
    std::unordered_set<pid_t> stopped_pids;

    const auto process_wait_signals = [&] {
        // this waiter owns the foreground children's status reports. reaping
        // a stop elsewhere could leave us waiting for a child that cannot run.
        if (shell) {
            (void)shell->process_pending_signals(false);
        } else if (auto* signal_handler = SignalHandler::instance()) {
            (void)signal_handler->process_pending_signals(this, false);
        }
    };
    while (!remaining_pids.empty()) {
        // signals received during launch will not interrupt a later waitpid.
        process_wait_signals();
        if (cjsh_env::exit_requested()) {
            last_exit_code = SignalHandler::termination_signal() != 0
                                 ? 128 + SignalHandler::termination_signal()
                                 : 0;
            return;
        }
        const pid_t wait_target = process_group ? -job_pgid : remaining_pids.front();
        pid = waitpid(wait_target, &status, WUNTRACED | WCONTINUED);

        if (pid == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                remaining_pids.clear();
                break;
            }
            set_error(ErrorType::RUNTIME_ERROR, "waitpid",
                      "failed to wait for child process: " + std::string(strerror(errno)));
            break;
        }

        auto pid_it = std::find(remaining_pids.begin(), remaining_pids.end(), pid);
        if (pid_it != remaining_pids.end() && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
            SignalHandler::note_startup_interrupt();
        }
        if (pid_it != remaining_pids.end() && (WIFEXITED(status) || WIFSIGNALED(status))) {
            (void)remaining_pids.erase(pid_it);
            stopped_pids.erase(pid);
        }

        // status arrival order is unrelated to pipeline order. only final child
        // results populate numeric pipeline statuses, not stop/continue reports.
        auto order_it = std::find(pid_order.begin(), pid_order.end(), pid);
        if (order_it != pid_order.end()) {
            size_t index = static_cast<size_t>(std::distance(pid_order.begin(), order_it));
            if (index < pipeline_statuses.size() && (WIFEXITED(status) || WIFSIGNALED(status))) {
                pipeline_statuses[index] = extract_exit_code(status);
            }
        }

        if (pid == last_pid) {
        }

        if (pid == last_pid) {
            saw_last = true;
            last_status = status;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            JobManager::instance().handle_child_status(pid, status);
        }

        // a pipeline is stopped only after every remaining member has stopped.
        // finished members no longer prevent that aggregate transition.
        if (WIFSTOPPED(status)) {
            stopped_pids.insert(pid);
            stop_signal = WSTOPSIG(status);
            JobManager::instance().handle_child_status(pid, status);
            if (!remaining_pids.empty() && stopped_pids.size() >= remaining_pids.size()) {
                job_stopped = true;
                break;
            }
        } else if (WIFCONTINUED(status)) {
            stopped_pids.erase(pid);
            JobManager::instance().handle_child_status(pid, status);
        }

        if (!remaining_pids.empty() && stopped_pids.size() >= remaining_pids.size()) {
            job_stopped = true;
            break;
        }
    }

    lock.lock();

    it = jobs.find(job_id);
    if (it != jobs.end()) {
        Job& job = it->second;
        if (!pipeline_statuses.empty()) {
            job.pipeline_statuses = std::move(pipeline_statuses);
        }

        if (job_stopped) {
            job.stopped = true;
            job.status = status;

            auto job_control = JobManager::instance().get_job_by_pid_or_pgid(job_pgid);

            // native auto-background behavior applies only to terminal-stop
            // requests, not every stop cause such as a background tty read.
            const bool should_auto_background =
                job.auto_background_on_stop && stop_signal == SIGTSTP && job.pgid > 0;

            if (should_auto_background) {
                if (job.auto_background_on_stop_silent && job.output_relay) {
                    job.output_relay->forward.store(false);
                }
                resume_job(job, true, "background job");
                job.background = true;
                job.completed = false;
                last_exit_code = 0;

                if (job_control) {
                    job_control->state.store(JobState::RUNNING, std::memory_order_relaxed);
                    job_control->background.store(true, std::memory_order_relaxed);
                    job_control->stop_notified.store(false, std::memory_order_relaxed);
                    job_control->stopped_pids.clear();
                    job_control->stop_signal = 0;
                    job_control->defer_stop_notification = false;
                }

                JobManager::instance().set_last_background_pid(job.last_pid);

                const std::string& display_command =
                    job_control ? job_control->display_command() : job.command;
                std::cerr << "\n[" << job_id << "]+ " << display_command << " &" << '\n';
            } else {
                last_exit_code = 128 + SIGTSTP;
                if (job_control) {
                    job_control->defer_stop_notification = false;
                    JobManager::instance().notify_job_stopped(job_control);
                }
            }
        } else {
            job.completed = true;
            job.stopped = false;
            job.status = status;

            int final_status = saw_last ? last_status : status;
            const bool exited_or_signaled = WIFEXITED(final_status) || WIFSIGNALED(final_status);
            if (!exited_or_signaled) {
                final_status = (job.last_status != 0) ? job.last_status : job.status;
            }
            job.last_status = final_status;

            if (WIFEXITED(final_status) || WIFSIGNALED(final_status)) {
                last_exit_code = extract_exit_code(final_status);
                job.completed = true;
                set_error_from_wait_status(job.command, final_status);
            }
        }
    }
}

// apply an already consumed wait report from a normal-context reaper. despite
// the name, this is not an asynchronous signal callback: it takes a mutex and may
// allocate. keep pipeline ordering even as final reports remove live pids.
void Exec::handle_child_signal(pid_t pid, int status) {
    static bool use_signal_masking = false;
    static int signal_count = 0;

    if (++signal_count > 10) {
        use_signal_masking = true;
    }

    std::unique_ptr<SignalMask> mask;
    if (use_signal_masking) {
        mask = std::make_unique<SignalMask>(SIGCHLD);
    }

    std::lock_guard<std::mutex> lock(jobs_mutex);

    for (auto& job_pair : jobs) {
        Job& job = job_pair.second;

        auto it = std::find(job.pids.begin(), job.pids.end(), pid);
        if (it != job.pids.end()) {
            auto order_it = std::find(job.pid_order.begin(), job.pid_order.end(), pid);
            if (order_it != job.pid_order.end()) {
                size_t index = static_cast<size_t>(std::distance(job.pid_order.begin(), order_it));
                int recorded = -1;
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    recorded = extract_exit_code(status);
                }
                if (recorded != -1 && index < job.pipeline_statuses.size()) {
                    job.pipeline_statuses[index] = recorded;
                }
            }
            if (pid == job.last_pid) {
                job.last_status = status;
            }
            if (WIFSTOPPED(status)) {
                job.stopped = true;
                job.status = status;
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                (void)job.pids.erase(it);

                if (job.pids.empty()) {
                    job.completed = true;
                    job.stopped = false;
                    job.status = status;
                }
            }
            break;
        }
    }
}

// return a snapshot so callers need not retain jobs_mutex while inspecting jobs.
std::map<int, Job> Exec::get_jobs() {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    return jobs;
}

// signal a snapshot during shell teardown, then give children a bounded chance
// to exit. disown-style hangup protection applies to SIGHUP, not all shutdown causes.
void Exec::terminate_all_child_process(int signal) {
    std::vector<Job> job_snapshot;
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
        for (pid_t pid : job.pids) {
            if (pid > 0) {
                (void)kill(pid, signum);
            }
        }
    };

    std::vector<pid_t> pending_children;
    for (const Job& job : job_snapshot) {
        if (job.completed || (signal == SIGHUP && job.hup_protected)) {
            continue;
        }
        send_signal_to_job(job, signal);
        pending_children.insert(pending_children.end(), job.pids.begin(), job.pids.end());
#ifdef SIGCONT
        // queue the shutdown signal before resuming a stopped job.
        if (job.stopped) {
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
        if (it->second.pgid == pgid) {
            jobs.erase(it);
            return;
        }
    }
}

void Exec::set_job_hup_protected(pid_t pgid, bool protected_from_hup) {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    for (auto& [id, job] : jobs) {
        (void)id;
        if (job.pgid == pgid) {
            job.hup_protected = protected_from_hup;
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
