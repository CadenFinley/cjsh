/*
  signal_handler.cpp

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

#include "signal_handler.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "error_out.h"
#include "exec.h"
#include "isocline.h"
#include "job_control.h"
#include "numeric_utils.h"
#include "pipeline_status_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "trap_command.h"

// separate asynchronous signal receipt from shell work at execution safe points.
// foreground waiters can dispatch traps without surrendering child wait statuses;
// shutdown coordination stays live until cjsh.cpp has released shell resources.

// restore the caller's exact mask on scope exit, including signals that were
// already blocked. a failed block operation must not trigger a later restoration.
SignalMask::SignalMask(int signum) : active(false) {
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) == 0) {
        active = true;
    }
}

SignalMask::SignalMask(const std::vector<int>& signals) : active(false) {
    if (signals.empty()) {
        return;
    }
    sigset_t mask{};
    sigemptyset(&mask);
    for (int sig : signals) {
        sigaddset(&mask, sig);
    }
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) == 0) {
        active = true;
    }
}

SignalMask::~SignalMask() {
    if (active) {
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
    }
}

std::atomic<SignalHandler*> SignalHandler::s_instance(nullptr);

SignalHandler* SignalHandler::instance() {
    return s_instance.load(std::memory_order_acquire);
}

// these flags coalesce receipt rather than counting deliveries. direct flags also
// keep work visible when a consumer clears the shared pending hint but leaves a
// specific action, such as child reaping, for its owning waiter.
volatile sig_atomic_t SignalHandler::s_sigint_received = 0;
volatile sig_atomic_t SignalHandler::s_startup_interrupt_received = 0;
volatile sig_atomic_t SignalHandler::s_sigchld_received = 0;
volatile sig_atomic_t SignalHandler::s_sighup_received = 0;
volatile sig_atomic_t SignalHandler::s_sigterm_received = 0;
volatile sig_atomic_t SignalHandler::s_sigquit_received = 0;
volatile sig_atomic_t SignalHandler::s_sigtstp_received = 0;
volatile sig_atomic_t SignalHandler::s_sigusr1_received = 0;
volatile sig_atomic_t SignalHandler::s_sigusr2_received = 0;
volatile sig_atomic_t SignalHandler::s_sigabrt_received = 0;
volatile sig_atomic_t SignalHandler::s_sigalrm_received = 0;
volatile sig_atomic_t SignalHandler::s_sigwinch_received = 0;
volatile sig_atomic_t SignalHandler::s_sigpipe_received = 0;
volatile sig_atomic_t SignalHandler::s_sigttin_received = 0;
volatile sig_atomic_t SignalHandler::s_sigttou_received = 0;
volatile sig_atomic_t SignalHandler::s_sigcont_received = 0;

std::atomic<bool> SignalHandler::s_signal_pending(false);
pid_t SignalHandler::s_main_pid = 0;
volatile sig_atomic_t SignalHandler::s_termination_signal = 0;
volatile sig_atomic_t SignalHandler::s_shutting_down = 0;
bool SignalHandler::s_executing_trap = false;
std::unordered_map<int, struct sigaction> SignalHandler::s_inherited_actions;
volatile sig_atomic_t SignalHandler::s_observed_signals[NSIG] = {};
std::unordered_map<int, SignalState> SignalHandler::s_signal_states;

// share platform-supported names and capabilities with trap and job builtins.
// pseudo-traps are added separately; they are not operating-system signals.
const std::vector<SignalInfo>& SignalHandler::signal_table() {
    static const std::vector<SignalInfo> kSignalTable = {
#ifdef SIGHUP
        {SIGHUP, "SIGHUP", "Terminal hung up", true, true},
#endif
#ifdef SIGINT
        {SIGINT, "SIGINT", "Interrupt (Ctrl+C)", true, true},
#endif
#ifdef SIGQUIT
        {SIGQUIT, "SIGQUIT", "Quit with core dump (Ctrl+\\)", true, true},
#endif
#ifdef SIGILL
        {SIGILL, "SIGILL", "Illegal instruction", true, true},
#endif
#ifdef SIGTRAP
        {SIGTRAP, "SIGTRAP", "Trace/breakpoint trap", true, true},
#endif
#ifdef SIGABRT
        {SIGABRT, "SIGABRT", "Abort", true, true},
#endif
#ifdef SIGBUS
        {SIGBUS, "SIGBUS", "Bus error (misaligned address)", true, true},
#endif
#ifdef SIGFPE
        {SIGFPE, "SIGFPE", "Floating point exception", true, true},
#endif
#ifdef SIGKILL
        {SIGKILL, "SIGKILL", "Kill (cannot be caught or ignored)", false, false},
#endif
#ifdef SIGUSR1
        {SIGUSR1, "SIGUSR1", "User-defined signal 1", true, true},
#endif
#ifdef SIGUSR2
        {SIGUSR2, "SIGUSR2", "User-defined signal 2", true, true},
#endif
#ifdef SIGSEGV
        {SIGSEGV, "SIGSEGV", "Segmentation violation", true, true},
#endif
#ifdef SIGPIPE
        {SIGPIPE, "SIGPIPE", "Broken pipe", true, true},
#endif
#ifdef SIGALRM
        {SIGALRM, "SIGALRM", "Alarm clock", true, true},
#endif
#ifdef SIGTERM
        {SIGTERM, "SIGTERM", "Termination", true, true},
#endif
#ifdef SIGCHLD
        {SIGCHLD, "SIGCHLD", "Child status changed", true, true},
#endif
#ifdef SIGCONT
        {SIGCONT, "SIGCONT", "Continue executing if stopped", true, true},
#endif
#ifdef SIGSTOP
        {SIGSTOP, "SIGSTOP", "Stop executing (cannot be caught or ignored)", false, false},
#endif
#ifdef SIGTSTP
        {SIGTSTP, "SIGTSTP", "Terminal stop (Ctrl+Z)", true, true},
#endif
#ifdef SIGTTIN
        {SIGTTIN, "SIGTTIN", "Background process trying to read from TTY", true, true},
#endif
#ifdef SIGTTOU
        {SIGTTOU, "SIGTTOU", "Background process trying to write to TTY", true, true},
#endif
#ifdef SIGURG
        {SIGURG, "SIGURG", "Urgent condition on socket", true, true},
#endif
#ifdef SIGXCPU
        {SIGXCPU, "SIGXCPU", "CPU time limit exceeded", true, true},
#endif
#ifdef SIGXFSZ
        {SIGXFSZ, "SIGXFSZ", "File size limit exceeded", true, true},
#endif
#ifdef SIGVTALRM
        {SIGVTALRM, "SIGVTALRM", "Virtual timer expired", true, true},
#endif
#ifdef SIGPROF
        {SIGPROF, "SIGPROF", "Profiling timer expired", true, true},
#endif
#ifdef SIGWINCH
        {SIGWINCH, "SIGWINCH", "Window size change", true, true},
#endif
#ifdef SIGIO
        {SIGIO, "SIGIO", "I/O now possible", true, true},
#endif
#ifdef SIGPWR
        {SIGPWR, "SIGPWR", "Power failure restart", true, true},
#endif
#ifdef SIGSYS
        {SIGSYS, "SIGSYS", "Bad system call", true, true},
#endif
#ifdef SIGINFO
        {SIGINFO, "SIGINFO", "Information request", true, true},
#endif
    };

    return kSignalTable;
}

const std::vector<SignalInfo>& SignalHandler::available_signals() {
    return signal_table();
}

SignalHandler::SignalHandler() {
    // children can receive signals before exec resets their handlers. record
    // the owning shell now, before any fork, so those signals keep child defaults.
    s_main_pid = getpid();
    s_startup_interrupt_received = 0;
    s_inherited_actions.clear();
    s_signal_states.clear();
    for (auto& observed : s_observed_signals) {
        observed = 0;
    }
    // capture inherited dispositions before installing shell policy. inherited
    // ignores must remain distinguishable from ignores used only by the shell.
    for (const auto& info : signal_table()) {
        struct sigaction action{};
        if (sigaction(info.signal, nullptr, &action) == 0) {
            s_inherited_actions[info.signal] = action;
        }
    }
    signal_unblock_all();
    s_instance.store(this);
}

bool SignalHandler::inherited_ignored(int signum) {
    auto it = s_inherited_actions.find(signum);
    return it != s_inherited_actions.end() && it->second.sa_handler == SIG_IGN;
}

// children retain inherited and user-requested ignores, but not shell-only SYSTEM
// dispositions such as ignoring SIGPIPE while a builtin writes to a closed pipe.
bool SignalHandler::child_ignored(int signum) {
    auto it = s_signal_states.find(signum);
    return inherited_ignored(signum) ||
           (it != s_signal_states.end() && it->second.disposition == SignalDisposition::IGNORE);
}

// prepare a child or exec replacement without carrying shell handlers or blocked
// launch signals into the new program. preserve ignores selected by child policy.
void reset_child_signals() {
    for (const auto& info : SignalHandler::available_signals()) {
        if (info.can_trap) {
            (void)signal(info.signal,
                         SignalHandler::child_ignored(info.signal) ? SIG_IGN : SIG_DFL);
        }
    }
    sigset_t set{};
    sigemptyset(&set);
    (void)sigprocmask(SIG_SETMASK, &set, nullptr);
}

// exec runs in the shell process, so save dispositions and the mask before using
// child policy. successful exec never returns; failure unwinds this guard instead.
ExecSignalGuard::ExecSignalGuard() {
    (void)sigprocmask(SIG_SETMASK, nullptr, &mask);
    for (const auto& info : SignalHandler::available_signals()) {
        struct sigaction action{};
        if (info.can_trap && sigaction(info.signal, nullptr, &action) == 0) {
            actions.emplace_back(info.signal, action);
        }
    }
    reset_child_signals();
}

ExecSignalGuard::~ExecSignalGuard() {
    for (const auto& [signum, action] : actions) {
        (void)sigaction(signum, &action, nullptr);
    }
    (void)sigprocmask(SIG_SETMASK, &mask, nullptr);
}

int SignalHandler::termination_signal() {
    return s_termination_signal;
}

// cjsh.cpp blocks terminating signals around the transition into shutdown. later
// untrapped termination is recorded without scheduling another cleanup sequence.
void SignalHandler::begin_shutdown() {
    s_shutting_down = 1;
}

bool SignalHandler::shutting_down() {
    return s_shutting_down != 0;
}

void SignalHandler::finish_shutdown() {
    // cleanup is complete. signals arriving after this point can take their
    // default action directly, without touching shell objects during teardown.
    for (int signum : {SIGHUP, SIGTERM}) {
        if (!child_ignored(signum) && !is_signal_observed(signum)) {
            (void)signal(signum, SIG_DFL);
        }
    }
    const int signum = s_termination_signal;
    if (signum == 0) {
        return;
    }
    // re-raise after cleanup so the parent can observe signal death rather than
    // only a numeric exit code. _exit is the fallback if delivery does not end us.
    (void)fflush(nullptr);
    (void)signal(signum, SIG_DFL);
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    (void)sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    (void)kill(getpid(), signum);
    _exit(128 + signum);
}

bool SignalHandler::has_direct_pending_signal() {
    return s_sigint_received != 0 || s_sigchld_received != 0 || s_sighup_received != 0 ||
           s_sigterm_received != 0 || s_sigttin_received != 0 || s_sigttou_received != 0 ||
           s_sigquit_received != 0 || s_sigtstp_received != 0 || s_sigusr1_received != 0 ||
           s_sigusr2_received != 0 || s_sigabrt_received != 0 || s_sigalrm_received != 0 ||
           s_sigcont_received != 0 || s_sigwinch_received != 0 || s_sigpipe_received != 0;
}

bool SignalHandler::has_pending_signals() {
    return s_signal_pending.load(std::memory_order_acquire) || has_direct_pending_signal();
}

bool SignalHandler::has_pending_termination_signal() {
    return s_sighup_received != 0 || s_sigterm_received != 0;
}

bool SignalHandler::take_pending_sigint() {
    if (s_sigint_received == 0) {
        return false;
    }

    s_sigint_received = 0;
    return true;
}

SignalHandler::~SignalHandler() {
    restore_original_handlers();
    s_instance.store(nullptr);
}

int SignalHandler::name_to_signal(const std::string& name) {
    std::string search_name = name;

    if (search_name.size() > 3 &&
        (search_name.substr(0, 3) == "SIG" || search_name.substr(0, 3) == "sig")) {
        search_name = search_name.substr(3);
    }

    for (char& c : search_name) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    for (const auto& signal : signal_table()) {
        std::string signal_name = signal.name;
        if (signal_name.size() > 3 && signal_name.substr(0, 3) == "SIG") {
            signal_name = signal_name.substr(3);
        }

        if (signal_name == search_name) {
            return signal.signal;
        }
    }

    int signal_number = 0;
    return numeric_utils::parse_int_strict(name, signal_number) ? signal_number : -1;
}

// reserve internal ids for shell events before validating real signal names.
// these pseudo-traps never pass through sigaction or the asynchronous handler.
int SignalHandler::parse_trap_signal_token(const std::string& token) {
    std::string search_name = token;
    for (char& c : search_name) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    if (search_name == "EXIT" || token == "0") {
        return 0;
    }
    if (search_name == "ERR") {
        return -2;
    }
    if (search_name == "DEBUG") {
        return -3;
    }
    if (search_name == "RETURN") {
        return -4;
    }

    int signal = name_to_signal(search_name);
    if (signal <= 0 && token != "0") {
        return -1;
    }
    if (signal == 0) {
        return 0;
    }
    if (!is_valid_signal(signal)) {
        return -1;
    }
    return signal;
}

std::string SignalHandler::signal_to_name(int signum, bool strip_sig_prefix) {
    for (const auto& signal : signal_table()) {
        if (signal.signal != signum || signal.name == nullptr) {
            continue;
        }

        std::string name(signal.name);
        if (strip_sig_prefix && name.rfind("SIG", 0) == 0) {
            return name.substr(3);
        }
        return name;
    }

    return std::to_string(signum);
}

std::vector<std::pair<int, std::string>> SignalHandler::trap_signal_names() {
    std::vector<std::pair<int, std::string>> names;
    const auto& table = signal_table();
    names.reserve(table.size() + 4);
    for (const auto& signal : table) {
        if (signal.name == nullptr) {
            continue;
        }
        (void)names.emplace_back(signal.signal, signal_to_name(signal.signal, true));
    }

    (void)names.emplace_back(0, "EXIT");
    (void)names.emplace_back(-2, "ERR");
    (void)names.emplace_back(-3, "DEBUG");
    (void)names.emplace_back(-4, "RETURN");
    return names;
}

bool SignalHandler::is_valid_signal(int signum) {
    if (signum <= 0) {
        return false;
    }
    const auto& table = signal_table();
    return std::any_of(table.begin(), table.end(),
                       [signum](const auto& signal_info) { return signal_info.signal == signum; });
}

bool SignalHandler::can_trap_signal(int signum) {
    const auto& table = signal_table();
    return std::any_of(table.begin(), table.end(), [signum](const auto& signal) {
        return signal.signal == signum && signal.can_trap;
    });
}

bool SignalHandler::can_ignore_signal(int signum) {
    const auto& table = signal_table();
    return std::any_of(table.begin(), table.end(), [signum](const auto& signal) {
        return signal.signal == signum && signal.can_ignore;
    });
}

bool SignalHandler::is_forked_child() {
    pid_t current_pid = getpid();
    if (s_main_pid == 0) {
        s_main_pid = current_pid;
        return false;
    }
    return current_pid != s_main_pid;
}

void SignalHandler::signal_unblock_all() {
    sigset_t iset{};
    sigemptyset(&iset);
    (void)sigprocmask(SIG_SETMASK, &iset, nullptr);
}

// update kernel disposition and shell bookkeeping together. inherited ignores
// cannot be overridden here, and uncatchable signals must never reach installation.
void SignalHandler::set_signal_disposition(int signum, SignalDisposition disp, const std::string&) {
    if (!is_valid_signal(signum)) {
        return;
    }

    if (signum == SIGKILL || signum == SIGSTOP || inherited_ignored(signum)) {
        return;
    }

    SignalState& state = s_signal_states[signum];
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);

    switch (disp) {
        case SignalDisposition::DEFAULT: {
            sa.sa_handler = SIG_DFL;
            sa.sa_flags = 0;
            if (sigaction(signum, &sa, nullptr) == 0) {
                state.disposition = SignalDisposition::DEFAULT;
                unobserve_signal(signum);
            }
            break;
        }

        case SignalDisposition::IGNORE: {
            if (!can_ignore_signal(signum)) {
                return;
            }
            sa.sa_handler = SIG_IGN;
            sa.sa_flags = 0;
            if (sigaction(signum, &sa, nullptr) == 0) {
                state.disposition = SignalDisposition::IGNORE;
                unobserve_signal(signum);
            }
            break;
        }

        case SignalDisposition::TRAPPED: {
            if (!can_trap_signal(signum)) {
                return;
            }

            struct sigaction old_action{};
            install_signal_handler(signum, &old_action);
            state.original_action = old_action;
            state.disposition = SignalDisposition::TRAPPED;
            observe_signal(signum);

            break;
        }

        case SignalDisposition::SYSTEM: {
            struct sigaction sys_old_action{};
            install_signal_handler(signum, &sys_old_action);
            state.original_action = sys_old_action;
            state.disposition = SignalDisposition::SYSTEM;
            break;
        }
    }
}

void SignalHandler::ignore_signal(int signum) {
    set_signal_disposition(signum, SignalDisposition::IGNORE);
}

void SignalHandler::restore_signal_disposition(int signum, const struct sigaction& action) {
    if (sigaction(signum, &action, nullptr) != 0) {
        return;
    }
    auto& state = s_signal_states[signum];
    state.disposition = action.sa_handler == SIG_IGN   ? SignalDisposition::SYSTEM
                        : action.sa_handler == SIG_DFL ? SignalDisposition::DEFAULT
                                                       : SignalDisposition::SYSTEM;
    unobserve_signal(signum);
}

void SignalHandler::install_signal_handler(int signum, struct sigaction* old_action) {
    if (inherited_ignored(signum)) {
        if (old_action) {
            (void)sigaction(signum, nullptr, old_action);
        }
        return;
    }
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigfillset(&sa.sa_mask);

    sa.sa_flags = 0;

    // exit-causing signals must interrupt waitpid/read loops so deferred dispatch
    // can promptly reach managed jobs. SIGCHLD and cosmetic signals remain restartable.
    if (signum != SIGINT && signum != SIGHUP && signum != SIGTERM) {
        sa.sa_flags |= SA_RESTART;
    }

    (void)sigaction(signum, &sa, old_action);
}

// evaluate traps only from normal execution. prevent nested trap dispatch and
// restore the interrupted command's status rather than exposing the trap's result.
void SignalHandler::process_trapped_signal(int signum) {
    if (trap_manager_has_trap(signum) && !s_executing_trap) {
        const int saved_status =
            numeric_utils::parse_exit_status_or(cjsh_env::get_shell_variable_value("?"), 0, false);
        s_executing_trap = true;
        trap_manager_execute_trap(signum);
        s_executing_trap = false;
        pipeline_status_utils::set_last_status_env(saved_status);
    }
}

bool SignalHandler::executing_trap() {
    return s_executing_trap;
}

void SignalHandler::note_startup_interrupt() {
    // keep cancellation after a waiter or builtin consumes the pending SIGINT.
    // foreground children have their own process group, so their wait status can
    // be the only evidence of Ctrl-C available to the shell.
    if (config::interactive_mode && cjsh_env::startup_active() && !is_forked_child()) {
        s_startup_interrupt_received = 1;
    }
}

bool SignalHandler::startup_interrupted() {
    return s_startup_interrupt_received != 0 && cjsh_env::startup_active() && !is_forked_child();
}

// asynchronous entry point: do not add allocation, ordinary diagnostics, or trap
// evaluation here. record work and wake readline where needed; child defaults and
// immediate non-interactive interrupt/quit exits are handled directly below.
void SignalHandler::signal_handler(int signum) {
    // a child still carrying this handler must not mutate the parent's copied
    // shell state. restore the signal's default action and deliver it again.
    if (is_forked_child()) {
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        (void)sigaction(signum, &sa, nullptr);
        (void)raise(signum);
        return;
    }

    bool is_observed = is_signal_observed(signum);
    if ((signum == SIGHUP || signum == SIGTERM) && shutting_down()) {
        if (!is_observed && s_termination_signal == 0) {
            s_termination_signal = signum;
        }
        return;
    }
    bool should_mark_pending = is_observed;

    switch (signum) {
        case SIGINT: {
            s_sigint_received = 1;
            note_startup_interrupt();
            ic_notify_readline();

            if ((!is_observed) && (!config::interactive_mode)) {
                cjsh_env::request_exit();
                _exit(128 + SIGINT);
            }

            should_mark_pending = true;
            break;
        }

        case SIGCHLD: {
            s_sigchld_received = 1;
            ic_notify_readline();
            should_mark_pending = true;
            break;
        }

        case SIGHUP: {
            s_sighup_received = 1;
            ic_notify_readline();
            should_mark_pending = true;
            break;
        }

        case SIGTERM: {
            s_sigterm_received = 1;
            // dispatch traps and cleanup outside the asynchronous handler.
            ic_notify_readline();
            should_mark_pending = true;
            break;
        }

        case SIGQUIT: {
            s_sigquit_received = 1;

            if ((!is_observed) && (!config::interactive_mode)) {
                _exit(128 + SIGQUIT);
            }

            break;
        }

        case SIGTSTP: {
            s_sigtstp_received = 1;
            should_mark_pending = true;

            if (!config::interactive_mode && !is_observed) {
                // orphaned process groups can discard default SIGTSTP. force a
                // real stop for an untrapped non-interactive suspension request.
                (void)kill(getpid(), SIGSTOP);
            }

            break;
        }

#ifdef SIGUSR1
        case SIGUSR1: {
            s_sigusr1_received = 1;
            break;
        }
#endif

#ifdef SIGUSR2
        case SIGUSR2: {
            s_sigusr2_received = 1;
            break;
        }
#endif

#ifdef SIGABRT
        case SIGABRT: {
            s_sigabrt_received = 1;
            break;
        }
#endif

#ifdef SIGCONT
        case SIGCONT: {
            s_sigcont_received = 1;
            should_mark_pending = true;
            break;
        }
#endif

#ifdef SIGALRM
        case SIGALRM: {
            s_sigalrm_received = 1;
            break;
        }
#endif

#ifdef SIGWINCH
        case SIGWINCH: {
            s_sigwinch_received = 1;
            // this is currently disabled as this lets isocline know about terminal resizing, which
            // is currently broken.
            // ic_notify_resize();

            break;
        }
#endif

#ifdef SIGPIPE
        case SIGPIPE: {
            s_sigpipe_received = 1;

            if (!is_observed) {
                return;
            }
            break;
        }
#endif

#ifdef SIGTTIN
        case SIGTTIN: {
            s_sigttin_received = 1;
            break;
        }
#endif

#ifdef SIGTTOU
        case SIGTTOU: {
            s_sigttou_received = 1;
            break;
        }
#endif

        default: {
            break;
        }
    }

    if (should_mark_pending) {
        s_signal_pending.store(true, std::memory_order_release);
    }
}

// install shell process policy after its foreground startup handshake. ignored
// tty signals allow terminal handoff; child setup later restores child policy.
void SignalHandler::setup_signal_handlers() {
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sigset_t block_mask{};
    sigfillset(&block_mask);

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sa.sa_mask = block_mask;
    (void)sigaction(SIGPIPE, &sa, nullptr);

    s_signal_states[SIGPIPE].disposition = SignalDisposition::SYSTEM;

    (void)sigaction(SIGTTOU, &sa, nullptr);
    (void)sigaction(SIGTTIN, &sa, nullptr);

    install_signal_handler(SIGCHLD, nullptr);
    install_signal_handler(SIGINT, nullptr);
    install_signal_handler(SIGHUP, nullptr);
    install_signal_handler(SIGTERM, nullptr);

#ifdef SIGTSTP
    if (!config::interactive_mode) {
        install_signal_handler(SIGTSTP, nullptr);
        s_signal_states[SIGTSTP].disposition = SignalDisposition::SYSTEM;
    }
#endif

    s_signal_states[SIGCHLD].disposition = SignalDisposition::SYSTEM;
    s_signal_states[SIGHUP].disposition = SignalDisposition::SYSTEM;
    s_signal_states[SIGTERM].disposition = SignalDisposition::SYSTEM;

    s_signal_states[SIGINT].disposition = SignalDisposition::SYSTEM;
}

// interactive shells survive terminal quit/stop keys themselves; foreground jobs
// receive those keys under their own dispositions after terminal handoff.
void SignalHandler::setup_interactive_handlers() {
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sigset_t block_mask{};
    sigfillset(&block_mask);

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sa.sa_mask = block_mask;

    (void)sigaction(SIGQUIT, &sa, nullptr);
    (void)sigaction(SIGTSTP, &sa, nullptr);

#ifdef SIGWINCH

    install_signal_handler(SIGWINCH, nullptr);
    s_signal_states[SIGWINCH].disposition = SignalDisposition::SYSTEM;
#endif
}

// normal destruction can restore inherited policy, but shutdown deliberately
// leaves termination handling under finish_shutdown's control until cleanup ends.
void SignalHandler::restore_original_handlers() {
    if (shutting_down()) {
        return;
    }
    for (const auto& [signum, action] : s_inherited_actions) {
        (void)sigaction(signum, &action, nullptr);
    }
}

// one wait status must update both execution-side and user-facing job state.
// limit broad reaping per call so a burst of children does not monopolize dispatch.
void SignalHandler::reap_pending_children(Exec* shell_exec, bool managed_jobs_only) {
    if (shell_exec == nullptr || s_sigchld_received == 0) {
        return;
    }
    s_sigchld_received = 0;
    if (managed_jobs_only) {
        // prompt workers own their captured children. the editor must only poll shell jobs.
        JobManager::instance().update_job_statuses();
        return;
    }
    constexpr int max_reap_iterations = 100;
    for (int count = 0; count < max_reap_iterations;) {
        int status = 0;
        const pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid < 0 && errno == EINTR) {
            continue;
        }
        if (pid <= 0) {
            return;
        }
        ++count;
        shell_exec->handle_child_signal(pid, status);
        JobManager::instance().handle_child_status(pid, status);
    }
    // leave work pending after a burst; never consume a status beyond the batch limit.
    s_sigchld_received = 1;
    s_signal_pending.store(true, std::memory_order_release);
}

// consume pending work at a shell safe point. reap_children=false lets a blocking
// foreground waiter remain the sole consumer of its children's wait statuses;
// the unconsumed SIGCHLD flag keeps that work visible for a later reaper.
SignalProcessingResult SignalHandler::process_pending_signals(Exec* shell_exec,
                                                              bool reap_children) {
    bool should_process = s_signal_pending.exchange(false, std::memory_order_acq_rel);
    if (!should_process && !has_direct_pending_signal()) {
        return {};
    }

    SignalProcessingResult result{};

    if (s_sigint_received != 0) {
        s_sigint_received = 0;
        result.sigint = true;

        bool is_observed = is_signal_observed(SIGINT);

        if (!is_observed && (shell_exec != nullptr)) {
            auto jobs = shell_exec->get_jobs();
            for (const auto& job_pair : jobs) {
                const auto& job = job_pair.second;
                if (!job.background && !job.completed && !job.stopped) {
                    if (kill(-job.pgid, SIGINT) < 0) {
                        print_error_errno({ErrorType::RUNTIME_ERROR,
                                           "signal",
                                           "kill SIGINT in process_pending_signals",
                                           {}});
                    }
                    break;
                }
            }
        }

        (void)fflush(stdout);
    }

    if (reap_children) {
        reap_pending_children(shell_exec);
    }

    // a trap handles the signal without automatically exiting. untrapped hangup
    // or termination selects the first shutdown cause and requests orderly exit.
    const auto dispatch_termination = [&](int signum, bool& terminating) {
        if (is_signal_observed(signum)) {
            process_trapped_signal(signum);
            result.trapped_signals.push_back(signum);
        } else {
            terminating = true;
            if (s_termination_signal == 0) {
                s_termination_signal = signum;
            }
            cjsh_env::request_exit();
        }
    };
    if (s_sighup_received != 0) {
        s_sighup_received = 0;
        dispatch_termination(SIGHUP, result.sighup);
    }
    if (s_sigterm_received != 0) {
        s_sigterm_received = 0;
        dispatch_termination(SIGTERM, result.sigterm);
    }

    if (s_sigquit_received != 0) {
        s_sigquit_received = 0;
        if (is_signal_observed(SIGQUIT)) {
            process_trapped_signal(SIGQUIT);
            result.trapped_signals.push_back(SIGQUIT);
        }
    }

    if (s_sigtstp_received != 0) {
        s_sigtstp_received = 0;
        if (is_signal_observed(SIGTSTP)) {
            process_trapped_signal(SIGTSTP);
            result.trapped_signals.push_back(SIGTSTP);
        }
    }

#ifdef SIGUSR1
    if (s_sigusr1_received != 0) {
        s_sigusr1_received = 0;
        if (is_signal_observed(SIGUSR1)) {
            process_trapped_signal(SIGUSR1);
            result.trapped_signals.push_back(SIGUSR1);
        }
    }
#endif

#ifdef SIGUSR2
    if (s_sigusr2_received != 0) {
        s_sigusr2_received = 0;
        if (is_signal_observed(SIGUSR2)) {
            process_trapped_signal(SIGUSR2);
            result.trapped_signals.push_back(SIGUSR2);
        }
    }
#endif

#ifdef SIGABRT
    if (s_sigabrt_received != 0) {
        s_sigabrt_received = 0;
        if (is_signal_observed(SIGABRT)) {
            process_trapped_signal(SIGABRT);
            result.trapped_signals.push_back(SIGABRT);
        }
    }
#endif

#ifdef SIGCONT
    if (s_sigcont_received != 0) {
        s_sigcont_received = 0;

        if (is_signal_observed(SIGCONT)) {
            process_trapped_signal(SIGCONT);
            result.trapped_signals.push_back(SIGCONT);
        }
    }
#endif

#ifdef SIGALRM
    if (s_sigalrm_received != 0) {
        s_sigalrm_received = 0;
        if (is_signal_observed(SIGALRM)) {
            process_trapped_signal(SIGALRM);
            result.trapped_signals.push_back(SIGALRM);
        }
    }
#endif

#ifdef SIGWINCH
    if (s_sigwinch_received != 0) {
        s_sigwinch_received = 0;
        if (is_signal_observed(SIGWINCH)) {
            process_trapped_signal(SIGWINCH);
            result.trapped_signals.push_back(SIGWINCH);
        }
    }
#endif

#ifdef SIGPIPE
    if (s_sigpipe_received != 0) {
        s_sigpipe_received = 0;
        if (is_signal_observed(SIGPIPE)) {
            process_trapped_signal(SIGPIPE);
            result.trapped_signals.push_back(SIGPIPE);
        }
    }
#endif

#ifdef SIGTTIN
    if (s_sigttin_received != 0) {
        s_sigttin_received = 0;
        if (is_signal_observed(SIGTTIN)) {
            process_trapped_signal(SIGTTIN);
            result.trapped_signals.push_back(SIGTTIN);
        }
    }
#endif

#ifdef SIGTTOU
    if (s_sigttou_received != 0) {
        s_sigttou_received = 0;
        if (is_signal_observed(SIGTTOU)) {
            process_trapped_signal(SIGTTOU);
            result.trapped_signals.push_back(SIGTTOU);
        }
    }
#endif

    if (result.sigint && is_signal_observed(SIGINT)) {
        process_trapped_signal(SIGINT);
        result.trapped_signals.push_back(SIGINT);
    }

    return result;
}

void SignalHandler::observe_signal(int signum) {
    if (signum > 0 && signum < NSIG) {
        s_observed_signals[signum] = 1;
    }
}

void SignalHandler::unobserve_signal(int signum) {
    if (signum > 0 && signum < NSIG) {
        s_observed_signals[signum] = 0;
    }
}

bool SignalHandler::is_signal_observed(int signum) {
    // this lookup also runs in the asynchronous handler: no container traversal.
    return signum > 0 && signum < NSIG && s_observed_signals[signum] != 0;
}
