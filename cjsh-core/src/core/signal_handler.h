/*
  signal_handler.h

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

#ifndef CJSH_CORE_SRC_CORE_SIGNAL_HANDLER_H
#define CJSH_CORE_SRC_CORE_SIGNAL_HANDLER_H

#include <signal.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Exec;

struct SignalProcessingResult {
    bool sigint = false;
    bool sighup = false;
    bool sigterm = false;
    std::vector<int> trapped_signals;
};

class SignalMask {
   private:
    sigset_t old_mask{};
    bool active;

   public:
    explicit SignalMask(int signum);

    explicit SignalMask(const std::vector<int>& signals);

    ~SignalMask();

    SignalMask(const SignalMask&) = delete;
    SignalMask& operator=(const SignalMask&) = delete;
};
enum class SignalDisposition : std::uint8_t {
    DEFAULT,
    IGNORE,
    TRAPPED,
    SYSTEM
};

struct SignalInfo {
    int signal;
    const char* name;
    const char* description;
    bool can_trap;
    bool can_ignore;
};

struct SignalState {
    SignalDisposition disposition = SignalDisposition::DEFAULT;
    struct sigaction original_action{};
};

class SignalHandler {
   public:
    SignalHandler();
    ~SignalHandler();

    void signal_unblock_all();
    void setup_signal_handlers();
    void setup_interactive_handlers();

    SignalProcessingResult process_pending_signals(Exec* executor, bool reap_children = true);
    static bool has_pending_signals();
    static void reap_pending_children(Exec* executor, bool managed_jobs_only = false);
    static bool has_pending_termination_signal();
    static bool take_pending_sigint();
    static void note_startup_interrupt();
    static bool startup_interrupted();
    static bool inherited_ignored(int signum);
    static bool child_ignored(int signum);
    static int termination_signal();
    static void begin_shutdown();
    static bool shutting_down();
    static bool executing_trap();
    static void finish_shutdown();
    static SignalHandler* instance();
    static const std::vector<SignalInfo>& available_signals();

    static int name_to_signal(const std::string& name);
    static int parse_trap_signal_token(const std::string& token);
    static std::string signal_to_name(int signum, bool strip_sig_prefix = false);
    static std::vector<std::pair<int, std::string>> trap_signal_names();
    static bool is_valid_signal(int signum);
    static bool can_trap_signal(int signum);
    static bool can_ignore_signal(int signum);
    static bool is_forked_child();

    static void set_signal_disposition(int signum, SignalDisposition disp,
                                       const std::string& trap_command = "");
    static void ignore_signal(int signum);
    static void restore_signal_disposition(int signum, const struct sigaction& action);

    static void observe_signal(int signum);
    static void unobserve_signal(int signum);
    static bool is_signal_observed(int signum);

    static void signal_handler(int signum);
    static bool interrupt_pending() {
        return s_sigint_received != 0;
    }

   private:
    static std::atomic<SignalHandler*> s_instance;

    static volatile sig_atomic_t s_sigint_received;
    static volatile sig_atomic_t s_startup_interrupt_received;
    static volatile sig_atomic_t s_sigchld_received;
    static volatile sig_atomic_t s_sighup_received;
    static volatile sig_atomic_t s_sigterm_received;
    static volatile sig_atomic_t s_sigquit_received;
    static volatile sig_atomic_t s_sigtstp_received;
    static volatile sig_atomic_t s_sigusr1_received;
    static volatile sig_atomic_t s_sigusr2_received;
    static volatile sig_atomic_t s_sigabrt_received;
    static volatile sig_atomic_t s_sigalrm_received;
    static volatile sig_atomic_t s_sigwinch_received;
    static volatile sig_atomic_t s_sigpipe_received;
    static volatile sig_atomic_t s_sigttin_received;
    static volatile sig_atomic_t s_sigttou_received;
    static volatile sig_atomic_t s_sigcont_received;

    static std::atomic<bool> s_signal_pending;
    static bool has_direct_pending_signal();
    static const std::vector<SignalInfo>& signal_table();
    static pid_t s_main_pid;
    static volatile sig_atomic_t s_termination_signal;
    static volatile sig_atomic_t s_shutting_down;
    static bool s_executing_trap;
    static std::unordered_map<int, struct sigaction> s_inherited_actions;

    static std::unordered_map<int, SignalState> s_signal_states;
    static volatile sig_atomic_t s_observed_signals[NSIG];

    void restore_original_handlers();
    static void install_signal_handler(int signum, struct sigaction* old_action);
    static void process_trapped_signal(int signum);
};

void reset_child_signals();

// Restore the running shell's handlers and mask if exec returns with an error.
class ExecSignalGuard {
   public:
    ExecSignalGuard();
    ~ExecSignalGuard();
    ExecSignalGuard(const ExecSignalGuard&) = delete;
    ExecSignalGuard& operator=(const ExecSignalGuard&) = delete;

   private:
    std::vector<std::pair<int, struct sigaction>> actions;
    sigset_t mask{};
};

#endif  // CJSH_CORE_SRC_CORE_SIGNAL_HANDLER_H
