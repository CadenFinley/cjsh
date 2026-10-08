/*
  job_state.h

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

#ifndef CJSH_CORE_SRC_CORE_JOB_STATE_H
#define CJSH_CORE_SRC_CORE_JOB_STATE_H

#include <sys/types.h>
#include <termios.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct OutputRelayState {
    int master_fd{-1};
    std::atomic<bool> forward{true};
};

enum class JobState : std::uint8_t {
    RUNNING,
    STOPPED,
    DONE,
    TERMINATED
};

struct JobControlJob {
    int job_id = 0;
    pid_t pgid = 0;
    std::vector<pid_t> pids;
    std::unordered_set<pid_t> remaining_pids;
    std::unordered_set<pid_t> stopped_pids;
    pid_t last_pid{-1};
    std::string command;
    std::string command_name;
    bool auto_background_on_stop{false};
    bool auto_background_on_stop_silent{false};
    int status{0};       // Most recent stop or terminal wait report.
    int last_status{0};  // Terminal result of the last pipeline member.
    std::vector<int> pipeline_statuses;
    std::shared_ptr<OutputRelayState> output_relay;
    int launch_barrier_fd{-1};
    std::atomic<JobState> state{JobState::RUNNING};
    int exit_status{};
    int termination_signal{};
    int stop_signal{};
    bool notified{false};
    std::atomic<bool> stop_notified{false};
    std::atomic<bool> background{false};
    bool suppress_notifications{false};
    bool process_group{true};
    bool hup_protected{false};
    bool defer_stop_notification{false};
    bool reads_stdin{false};
    bool awaiting_stdin_signal{false};
    std::uint8_t last_stdin_signal{0};
    std::uint16_t stdin_signal_count{0};
    std::chrono::steady_clock::time_point last_stdin_signal_time{
        std::chrono::steady_clock::time_point::min()};
    std::string custom_name;
    struct termios tmodes{};
    bool tmodes_saved{false};

    JobControlJob() = default;
    JobControlJob(int id, pid_t group_id, const std::vector<pid_t>& process_ids,
                  const std::string& cmd, bool is_background, bool consumes_stdin,
                  bool has_process_group);

    bool completed() const {
        const auto current = state.load(std::memory_order_relaxed);
        return current == JobState::DONE || current == JobState::TERMINATED;
    }

    bool stopped() const {
        return state.load(std::memory_order_relaxed) == JobState::STOPPED;
    }

    void record_wait_status(pid_t pid, int wait_status);
    void mark_running();

    bool has_custom_name() const {
        return !custom_name.empty();
    }

    void set_custom_name(std::string name) {
        custom_name = std::move(name);
    }

    const std::string& display_command() const {
        return custom_name.empty() ? command : custom_name;
    }
};

using Job = JobControlJob;

#endif  // CJSH_CORE_SRC_CORE_JOB_STATE_H
