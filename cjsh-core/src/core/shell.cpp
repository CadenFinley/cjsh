/*
  shell.cpp

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

#include "shell.h"
#include <fcntl.h>
#include <sys/types.h>

#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "error_out.h"
#include "exec.h"
#include "flags.h"
#include "interpreter.h"
#include "isocline.h"
#include "job_control.h"
#include "numeric_utils.h"
#include "parser.h"
#include "pipeline_status_utils.h"
#include "prompt.h"
#include "readonly_command.h"
#include "script_dispatch.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "trap_command.h"

namespace {

constexpr size_t to_index(ShellOption option) {
    return static_cast<size_t>(option);
}

constexpr std::array<ShellOptionDescriptor, static_cast<size_t>(ShellOption::Count)>
    kShellOptionDescriptors = {{{ShellOption::Errexit, 'e', "errexit"},
                                {ShellOption::Noclobber, 'C', "noclobber"},
                                {ShellOption::Nounset, 'u', "nounset"},
                                {ShellOption::Xtrace, 'x', "xtrace"},
                                {ShellOption::Verbose, 'v', "verbose"},
                                {ShellOption::Noexec, 'n', "noexec"},
                                {ShellOption::Noglob, 'f', "noglob"},
                                {ShellOption::Globstar, 0, "globstar", true},
                                {ShellOption::Allexport, 'a', "allexport"},
                                {ShellOption::Huponexit, 0, "huponexit", true},
                                {ShellOption::Pipefail, 0, "pipefail"},
                                {ShellOption::Monitor, 'm', "monitor"},
                                {ShellOption::Hashall, 'h', "hashall"},
                                {ShellOption::Notify, 'b', "notify"},
                                {ShellOption::Ignoreeof, 0, "ignoreeof"},
                                {ShellOption::Nolog, 0, "nolog"},
                                {ShellOption::Extglob, 0, "extglob", true},
                                {ShellOption::ExpandAliases, 0, "expand_aliases", true},
                                {ShellOption::InheritErrexit, 0, "inherit_errexit", true},
                                {ShellOption::BraceExpand, 'B', "braceexpand"},
                                {ShellOption::HistExpand, 'H', "histexpand"}}};

struct ErrexitSeverityDescriptor {
    ErrorSeverity severity;
    const char* name;
};

constexpr std::array<ErrexitSeverityDescriptor, 4> kErrexitSeverityDescriptors = {
    {{ErrorSeverity::INFO, "info"},
     {ErrorSeverity::WARNING, "warning"},
     {ErrorSeverity::ERROR, "error"},
     {ErrorSeverity::CRITICAL, "critical"}}};

std::optional<ErrorSeverity> parse_errexit_severity_value(const std::string& value) {
    for (const auto& descriptor : kErrexitSeverityDescriptors) {
        if (value == descriptor.name) {
            return descriptor.severity;
        }
    }
    return std::nullopt;
}

const char* errexit_severity_name(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO:
            return "info";
        case ErrorSeverity::WARNING:
            return "warning";
        case ErrorSeverity::ERROR:
            return "error";
        case ErrorSeverity::CRITICAL:
            return "critical";
    }
    return "error";
}

}  // namespace

const std::array<ShellOptionDescriptor, static_cast<size_t>(ShellOption::Count)>&
get_shell_option_descriptors() {
    return kShellOptionDescriptors;
}

std::optional<ShellOption> parse_shell_option(const std::string& name) {
    for (const auto& descriptor : kShellOptionDescriptors) {
        if (!descriptor.shopt && name == descriptor.name) {
            return descriptor.option;
        }
    }
    return std::nullopt;
}

std::optional<ShellOption> parse_shopt_option(const std::string& name) {
    for (const auto& descriptor : kShellOptionDescriptors) {
        if (descriptor.shopt && name == descriptor.name) {
            return descriptor.option;
        }
    }
    return std::nullopt;
}

std::optional<ShellOption> parse_shell_option_short(char short_flag) {
    for (const auto& descriptor : kShellOptionDescriptors) {
        if (descriptor.short_flag == short_flag && descriptor.short_flag != 0) {
            return descriptor.option;
        }
    }
    return std::nullopt;
}

Shell::Shell() {
    trap_manager_initialize();

    // construct core subsystems before wiring them together
    shell_exec = std::make_unique<Exec>();
    signal_handler = std::make_unique<SignalHandler>();
    shell_parser = std::make_unique<Parser>();
    built_ins = std::make_unique<Built_ins>();
    shell_script_interpreter = std::make_unique<ShellScriptInterpreter>();

    // share references so the parser interpreter and builtins can coordinate through shell
    if (shell_script_interpreter && shell_parser) {
        shell_script_interpreter->set_parser(shell_parser.get());
        shell_parser->set_shell(this);
    }
    built_ins->set_shell(this);
    built_ins->set_current_directory();

    // use stdin as the controlling terminal for cjsh which is validated during job-control setup
    shell_terminal = STDIN_FILENO;

    // job and trap managers need every subsystem initialized before they attach to the shell
    JobManager::instance().set_shell(this);
    trap_manager_set_shell(this);

    // A nested interactive shell must let the kernel stop it with SIGTTIN until its parent
    // places it in the foreground.  Install the shell's ignored job-control dispositions only
    // after this handshake has completed.
    setup_job_control();

    // signal dispatch depends on the prior wiring so register handlers after setup completes
    setup_signal_handlers();
}

Shell::~Shell() {
    // on shell destruction, handle any remaining child processes
    if (shell_exec) {
        const int terminating_signal = SignalHandler::termination_signal();
        const bool hang_up_on_exit =
            get_shell_option(ShellOption::Huponexit) &&
            (!config::is_bash_mode() || (config::interactive_mode && config::login_mode));
        if (terminating_signal != 0 || hang_up_on_exit) {
            shell_exec->terminate_all_child_process(terminating_signal == SIGTERM ? SIGTERM
                                                                                  : SIGHUP);
        } else {
            shell_exec->abandon_all_child_processes();
        }
    }

    // after terminating or abandoning child processes, clear all get_jobs
    JobManager::instance().clear_all_jobs();

    // restore terminal state on exit to how we found it
    // again, if we restore it to a broken state, then we probably inherited a broken state
    restore_terminal_state();
    if (owns_shell_terminal) {
        (void)close(shell_terminal);
    }

    // output a final exit line only in interactive modes
    if (interactive_input_started && !cjsh_env::startup_active()) {
        if (config::login_mode) {
            std::cout << "cjsh logout";
        } else {
            std::cout << "cjsh exit";
        }
        (void)std::cout.flush();
    }
}

void Shell::run_exit_handlers(int status) {
    // A hook can launch another subshell. Its copy of this guard must prevent
    // recursively running the same exit handlers again.
    if (exit_handlers_invoked) {
        return;
    }
    exit_handlers_invoked = true;

    // Each exit handler starts with the original status and a cleared exit request.
    const auto prepare_handler = [status] {
        cjsh_env::clear_exit_request();
        pipeline_status_utils::set_last_status_env(status);
    };
    prepare_handler();
    trap_manager_set_shell(this);

    if (auto* interpreter = get_shell_script_interpreter();
        interpreter != nullptr && !config::minimal_mode && !config::secure_mode &&
        !config::is_posix_mode() && interpreter->has_function("cjshexit")) {
        (void)interpreter->invoke_function({"cjshexit"});
    }

    prepare_handler();
    trap_manager_execute_exit_trap();
    if (config::login_mode) {
        prepare_handler();
        cjsh_filesystem::process_logout_file();
    }
}

int Shell::execute(const std::string& script, bool skip_validation) {
    mark_terminal_dirty();
    // main execution entry point for cjsh
    if (script.empty()) {
        return 0;
    }

    // convert command into lines for execution
    std::vector<std::string> lines = shell_parser->parse_into_lines(script);

    if (shell_script_interpreter) {
        // execute the parsed lines
        // the block is tokenized, parsed, and interpreted and then passed to the execute_command
        // function
        int exit_code = shell_script_interpreter->execute_block(lines, skip_validation);
        last_command = script;
        return exit_code;
    }
    print_error(ErrorInfo{ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    return 1;
}

int Shell::execute_command(std::vector<std::string> args, bool run_in_background,
                           bool auto_background_on_stop, bool auto_background_on_stop_silent) {
    return execute_prepared_command(cjsh_env::prepare_command(std::move(args)), run_in_background,
                                    auto_background_on_stop, auto_background_on_stop_silent);
}

int Shell::execute_prepared_command(cjsh_env::PreparedCommand command, bool run_in_background,
                                    bool auto_background_on_stop,
                                    bool auto_background_on_stop_silent) {
    const auto& args = command.original_args;
    if (config::is_posix_mode()) {
        for (const auto& [name, value] : command.assignments) {
            if (!readonly_manager_can_assign(name, "assignment")) {
                return cjsh_env::posix_error_exit(1);
            }
        }
    }
    // fast path back out, this condition should never hit as many other things would have failed
    // beforehand
    if (!shell_exec || !built_ins) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }

    mark_terminal_dirty();
    // main single command executor that dirives from execute_block in interpreter.cpp
    if (args.empty()) {
        return 0;
    }

    // xtrace handling
    if (get_shell_option(ShellOption::Xtrace)) {
        std::cerr << prompt::render_trace_prompt() << string_utils::join_strings(args, " ") << '\n';
    }

    // noexec handling
    if (get_shell_option(ShellOption::Noexec)) {
        return 0;
    }

    // handle simple env var assignment with no command
    if (args.size() == 1 && shell_parser) {
        std::string var_name;
        std::string var_value;
        if (shell_parser->is_env_assignment(args[0], var_name, var_value)) {
            shell_parser->expand_env_vars(var_value);

            if (shell_script_interpreter) {
                shell_script_interpreter->get_variable_manager().set_environment_variable(
                    var_name, var_value);
            }

            return 0;
        }

        if (args[0].find('=') != std::string::npos) {
            return 1;
        }
    }

    // collect any env var assignments preceding the command
    const auto& env_assignments = command.assignments;
    const auto& command_args = command.args;
    const bool has_temporary_env = !env_assignments.empty() && !command_args.empty();
    const bool assignments_persist =
        has_temporary_env && !run_in_background && is_posix_special_builtin(command_args[0]);

    const bool is_direct_command =
        !command_args.empty() && (built_ins->is_builtin_or_runtime_command(command_args[0]) != 0);

    command.is_builtin = is_direct_command;

    // check for built-in and keyword-runtime command execution
    if (is_direct_command) {
        cjsh_env::TemporaryEnvAssignmentScope assignments(this, env_assignments,
                                                          assignments_persist);
        return built_ins->builtin_or_runtime_command(command_args);
    }

    // not a builtin check for other things
    if (interactive_mode && !run_in_background && command_args.size() == 1 && built_ins) {
        const std::string& candidate = command_args[0];

        if (command_lookup::should_auto_cd_token(candidate, this)) {
            std::vector<std::string> cd_args = {"cd", candidate};
            int code = built_ins->builtin_command(cd_args);
            return code;
        }
    }

    // execute the command in the background if requested
    if (run_in_background) {
        int job_id = shell_exec->execute_prepared_command_async(std::move(command));
        if (job_id > 0) {
            auto jobs = shell_exec->get_jobs();
            auto it = jobs.find(job_id);
            if (it != jobs.end() && !it->second.pids.empty()) {
                pid_t last_pid = it->second.pids.back();
                (void)setenv("!", std::to_string(last_pid).c_str(), 1);

                JobManager::instance().set_last_background_pid(last_pid);
            }
        }
        return 0;
    }

    // execute the command synchronously
    int exit_code = shell_exec->execute_prepared_command_sync(
        std::move(command), auto_background_on_stop, auto_background_on_stop_silent);
    shell_exec->print_error_if_needed(exit_code);
    return exit_code;
}

int Shell::execute_script_file(const std::filesystem::path& path, bool optional) {
    if (!shell_script_interpreter) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }

    std::filesystem::path normalized = path.lexically_normal();
    std::string display_path = normalized.string();

    std::error_code abs_ec;
    auto absolute_path = std::filesystem::absolute(normalized, abs_ec);
    if (!abs_ec) {
        normalized = absolute_path.lexically_normal();
        display_path = normalized.string();
    }

    std::ifstream file(normalized);
    if (!file) {
        if (optional) {
            return 0;
        }
        std::error_code status_ec;
        bool exists = std::filesystem::exists(normalized, status_ec);
        bool is_dir = !status_ec && std::filesystem::is_directory(normalized, status_ec);
        if (is_dir) {
            print_error(
                {ErrorType::RUNTIME_ERROR, "source", "is a directory: '" + display_path + "'", {}});
            return 1;
        }
        if (exists) {
            if (access(display_path.c_str(), R_OK) != 0 && errno == EACCES) {
                print_error({ErrorType::PERMISSION_DENIED,
                             "source",
                             "cannot open file '" + display_path + "'",
                             {}});
                return 1;
            }
        }
        print_error(
            {ErrorType::FILE_NOT_FOUND, "source", "cannot open file '" + display_path + "'", {}});
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return execute_script_content(buffer.str(), display_path);
}

int Shell::execute_script_content(const std::string& content, const std::string& source_path) {
    if (!shell_script_interpreter) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }
    const script_dispatch::BashScriptDialectScope dialect_scope(content);
    auto parsed_lines = shell_script_interpreter->parse_into_lines(content);
    if (parsed_lines.empty()) {
        return 0;
    }

    const std::string previous_error_source = shell_script_interpreter->get_error_source();
    shell_script_interpreter->set_error_source(source_path);
    shell_script_interpreter->push_source_scope();
    int exit_code = shell_script_interpreter->execute_block(parsed_lines);
    shell_script_interpreter->pop_source_scope();
    shell_script_interpreter->set_error_source(previous_error_source);

    if (exit_code == ShellScriptInterpreter::exit_return) {
        exit_code = numeric_utils::parse_exit_status_or(
            cjsh_env::get_shell_variable_value("CJSH_RETURN_CODE"), 0, false);
        (void)cjsh_env::unset_shell_variable_value("CJSH_RETURN_CODE");
        pipeline_status_utils::set_last_status_env(exit_code);
    }
    return exit_code;
}

int read_exit_code_or(int fallback) {
    std::string exit_code_str = cjsh_env::get_shell_variable_value("EXIT_CODE");
    if (exit_code_str.empty()) {
        return fallback;
    }

    fallback = numeric_utils::parse_exit_status_or(exit_code_str, fallback, false);
    (void)cjsh_env::unset_shell_variable_value("EXIT_CODE");
    return fallback;
}

SignalProcessingResult Shell::process_pending_signals(bool reap_children) {
    if (!signal_handler) {
        return {};
    }

    if (!SignalHandler::has_pending_signals()) {
        return {};
    }

    mark_terminal_dirty();
    Exec* exec_ptr = shell_exec ? shell_exec.get() : nullptr;
    return signal_handler->process_pending_signals(exec_ptr, reap_children);
}

void Shell::setup_signal_handlers() {
    signal_handler->setup_signal_handlers();
}

void Shell::setup_interactive_handlers() {
    signal_handler->setup_interactive_handlers();
}

void Shell::save_terminal_state() {
    if (interactive_job_control_available && (tcgetattr(shell_terminal, &shell_tmodes) == 0)) {
        terminal_state_saved = true;
    }
}

void Shell::restore_terminal_state() {
    if (terminal_state_saved && reclaim_terminal()) {
        if (tcsetattr(shell_terminal, TCSANOW, &shell_tmodes) != 0) {
            (void)tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
        }
        terminal_state_saved = false;
    }

    (void)fflush(stdout);
    (void)fflush(stderr);
}

void Shell::setup_job_control() {
    const bool requested_interactive = config::interactive_mode || config::force_interactive;
    if (!requested_interactive) {
        job_control_enabled = false;
        shell_options[to_index(ShellOption::Monitor)] = false;
        interactive_job_control_available = false;
        return;
    }
    // Stdio may point at a different PTY from /dev/tty (for example in a startup
    // benchmark). Prefer that terminal, keeping a private fd across redirections.
    int tty_fd = -1;
    for (const int fd : {STDIN_FILENO, STDOUT_FILENO}) {
        if (isatty(fd) == 0) {
            continue;
        }
        tty_fd = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
        if (tty_fd >= 0) {
            if (fcntl(tty_fd, F_SETFD, FD_CLOEXEC) == 0) {
                break;
            }
            (void)close(tty_fd);
            tty_fd = -1;
        }
    }
    if (tty_fd < 0) {
        tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    }
    if (tty_fd >= 0) {
        shell_terminal = tty_fd;
        owns_shell_terminal = true;
    }
    if (isatty(shell_terminal) == 0) {
        // Forced interactive execution without a controlling terminal still has job groups.
        job_control_enabled = true;
        shell_options[to_index(ShellOption::Monitor)] = true;
        interactive_job_control_available = false;
        return;
    }

    shell_pgid = getpgrp();

    // If cjsh was started in the background, wait until the parent shell foregrounds this
    // process group. An ignored SIGTTIN disposition survives exec, so explicitly restore the
    // default before using the signal for the standard foreground handshake. Login launchers
    // may create the shell's process group before assigning the terminal to it. In that case the
    // shell must claim the terminal itself: stopping here can deadlock with a launcher that only
    // waits for its login shell to exit.
    (void)signal(SIGTTIN, SIG_DFL);
    constexpr unsigned kMaxForegroundAttempts = 16;
    unsigned foreground_attempts = 0;
    for (;;) {
        const pid_t foreground_pgid = tcgetpgrp(shell_terminal);
        if (foreground_pgid < 0) {
            job_control_enabled = false;
            shell_options[to_index(ShellOption::Monitor)] = false;
            interactive_job_control_available = false;
            return;
        }
        if (foreground_pgid == shell_pgid) {
            break;
        }
        if (flags::is_login_shell_invocation()) {
            break;
        }
        // Orphaned process groups discard SIGTTIN. Bound repeated attempts,
        // without imposing a timeout on a shell stopped normally until `fg`.
        if (foreground_attempts++ == kMaxForegroundAttempts) {
            print_error({ErrorType::RUNTIME_ERROR,
                         ErrorSeverity::WARNING,
                         "startup",
                         "unable to acquire foreground terminal after repeated attempts",
                         {"Job control will remain disabled."}});
            job_control_enabled = false;
            shell_options[to_index(ShellOption::Monitor)] = false;
            interactive_job_control_available = false;
            return;
        }
        if (kill(-shell_pgid, SIGTTIN) < 0 && errno != EINTR) {
            job_control_enabled = false;
            shell_options[to_index(ShellOption::Monitor)] = false;
            interactive_job_control_available = false;
            return;
        }
        shell_pgid = getpgrp();
    }

    shell_pgid = getpid();

    // A session leader is already the leader of its process group and setpgid then reports
    // EPERM. Treat that as success only when the desired group is actually in place.
    if (setpgid(shell_pgid, shell_pgid) < 0 && getpgrp() != shell_pgid) {
        const auto error_text = std::system_category().message(errno);
        print_error({ErrorType::RUNTIME_ERROR,
                     ErrorSeverity::WARNING,
                     "setpgid",
                     "couldn't put the shell in its own process group: " + error_text,
                     {"Job control will remain disabled."}});
        job_control_enabled = false;
        shell_options[to_index(ShellOption::Monitor)] = false;
        interactive_job_control_available = false;
        return;
    }

    sigset_t sigttou_mask{};
    sigset_t previous_mask{};
    sigemptyset(&sigttou_mask);
    sigaddset(&sigttou_mask, SIGTTOU);
    const bool sigttou_blocked = sigprocmask(SIG_BLOCK, &sigttou_mask, &previous_mask) == 0;

    int foreground_result;
    do {
        foreground_result = tcsetpgrp(shell_terminal, shell_pgid);
    } while (foreground_result < 0 && errno == EINTR);
    const int foreground_error = errno;

    if (sigttou_blocked) {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    }

    if (foreground_result < 0) {
        errno = foreground_error;
        const auto error_text = std::system_category().message(errno);
        print_error({ErrorType::RUNTIME_ERROR,
                     ErrorSeverity::WARNING,
                     "tcsetpgrp",
                     "couldn't grab terminal control: " + error_text,
                     {"Job control will remain disabled."}});
        job_control_enabled = false;
        shell_options[to_index(ShellOption::Monitor)] = false;
        interactive_job_control_available = false;
        return;
    }

    interactive_job_control_available = true;
    job_control_enabled = true;
    shell_options[to_index(ShellOption::Monitor)] = true;
    save_terminal_state();
}

bool Shell::manages_terminal() const {
    return interactive_job_control_available && shell_pgid > 0 && getpid() == shell_pgid &&
           getpgrp() == shell_pgid;
}

void Shell::recover_prompt_terminal() {
    if (!prompt_terminal_dirty.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    if (!reclaim_terminal()) {
        mark_terminal_dirty();
        return;
    }
    // Clear before recovery so an asynchronous prompt worker cannot lose its invalidation.
    ic_recover_terminal();
}

bool Shell::reclaim_terminal() const {
    // Only the interactive shell that completed the startup foreground handshake may
    // reclaim this terminal. Forked subshells must not take it from their parent.
    // This remains necessary when the user disables monitor mode with `set +m`.
    if (!manages_terminal()) {
        return false;
    }

    pid_t foreground_pgid;
    do {
        foreground_pgid = tcgetpgrp(shell_terminal);
    } while (foreground_pgid < 0 && errno == EINTR);
    if (foreground_pgid < 0) {
        return false;
    }
    if (foreground_pgid == shell_pgid) {
        return true;
    }

    sigset_t sigttou_mask{};
    sigset_t previous_mask{};
    sigemptyset(&sigttou_mask);
    sigaddset(&sigttou_mask, SIGTTOU);
    if (sigprocmask(SIG_BLOCK, &sigttou_mask, &previous_mask) != 0) {
        return false;
    }

    int result;
    do {
        result = tcsetpgrp(shell_terminal, shell_pgid);
    } while (result < 0 && errno == EINTR);
    const int foreground_error = errno;
    (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    errno = foreground_error;
    return result == 0;
}

bool Shell::is_job_control_enabled() const {
    return job_control_enabled;
}

bool Shell::suspend() const {
    if (!manages_terminal()) {
        return false;
    }

    // Readline may be suspended by a widget; commands must expose ordinary
    // terminal modes while the parent shell handles the stop notification.
    const bool editor_active = ic_suspend_readline_terminal();
    ic_prepare_terminal_for_command();
    (void)fflush(stdout);
    (void)fflush(stderr);
    const bool stopped = kill(getpid(), SIGSTOP) == 0;

    // A background continuation must wait for fg instead of stealing the tty.
    struct sigaction previous{};
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTTIN, &action, &previous);
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTTIN);
    (void)sigprocmask(SIG_UNBLOCK, &mask, &old_mask);
    while (tcgetpgrp(shell_terminal) >= 0 && tcgetpgrp(shell_terminal) != getpgrp()) {
        if (kill(getpid(), SIGTTIN) != 0) {
            break;
        }
    }
    (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
    (void)sigaction(SIGTTIN, &previous, nullptr);
    if (editor_active) {
        (void)ic_resume_readline_terminal();
    }
    return stopped;
}

bool Shell::set_job_control_enabled(bool enabled) {
    // Monitor mode is still meaningful for a non-interactive shell: it controls process-group
    // creation even though there is no terminal to hand off. An interactive shell with a TTY
    // may only enable it after the startup foreground handshake succeeded.
    const bool requested_interactive = config::interactive_mode || config::force_interactive;
    if (enabled && requested_interactive && isatty(shell_terminal) != 0 &&
        !interactive_job_control_available) {
        return false;
    }
    job_control_enabled = enabled;
    shell_options[to_index(ShellOption::Monitor)] = enabled;
    return true;
}

void Shell::set_interactive_mode(bool flag) {
    if (interactive_mode == flag) {
        return;
    }

    interactive_mode = flag;

    if (interactive_mode) {
        apply_abbreviations_to_line_editor();
    } else {
        ic_clear_abbreviations();
    }
}

bool Shell::get_interactive_mode() const {
    return interactive_mode;
}

void Shell::set_abbreviations(
    const std::unordered_map<std::string, std::string>& new_abbreviations) {
    abbreviations = new_abbreviations;
    apply_abbreviations_to_line_editor();
}

std::unordered_map<std::string, std::string>& Shell::get_abbreviations() {
    return abbreviations;
}

void Shell::set_aliases(const std::unordered_map<std::string, std::string>& new_aliases) {
    aliases = new_aliases;
    if (shell_parser) {
        shell_parser->set_aliases(aliases);
    }
}

std::unordered_map<std::string, std::string>& Shell::get_aliases() {
    return aliases;
}

std::vector<std::string>& Shell::get_directory_stack() {
    return directory_stack;
}

const std::vector<std::string>& Shell::get_directory_stack() const {
    return directory_stack;
}

void Shell::set_last_interactive_command(const std::string& command) {
    last_interactive_command = command;
}

const std::string& Shell::get_last_interactive_command() const {
    return last_interactive_command;
}

void Shell::apply_abbreviations_to_line_editor() {
    if (!interactive_mode) {
        return;
    }

    ic_clear_abbreviations();
    for (const auto& [name, expansion] : abbreviations) {
        (void)ic_add_abbreviation(name.c_str(), expansion.c_str());
    }
}

void Shell::apply_startup_options(const std::vector<std::pair<std::string, bool>>& options) {
    for (const auto& [name, enabled] : options) {
        auto option = parse_shell_option(name);
        if (!option) {
            option = parse_shopt_option(name);
        }
        if (option) {
            set_shell_option(*option, enabled);
        }
    }
}

void Shell::set_shell_option(ShellOption option, bool value) {
    explicit_shell_options[to_index(option)] = true;
    if (option == ShellOption::Extglob) {
        config::extglob_enabled = value && !config::is_posix_mode();
    } else if (option == ShellOption::HistExpand) {
        config::history_expansion_enabled = value;
    }
    if (option == ShellOption::Monitor) {
        (void)set_job_control_enabled(value);
        return;
    }
    shell_options[to_index(option)] = value;
}

bool Shell::get_shell_option(ShellOption option) const {
    if (option == ShellOption::Extglob) {
        return config::extglob_enabled;
    }
    if (option == ShellOption::HistExpand) {
        return config::history_expansion_enabled;
    }
    if (config::is_posix_mode() &&
        (option == ShellOption::Globstar || option == ShellOption::BraceExpand)) {
        return false;
    }
    if (!explicit_shell_options[to_index(option)]) {
        switch (option) {
            case ShellOption::ExpandAliases:
                return !config::is_bash_mode() || interactive_mode;
            case ShellOption::InheritErrexit:
                return !config::is_bash_mode();
            case ShellOption::Huponexit:
                return !config::is_bash_mode() && config::interactive_mode;
            case ShellOption::BraceExpand:
            case ShellOption::Hashall:
                return !config::is_posix_mode();
            default:
                break;
        }
    }
    return shell_options[to_index(option)];
}

bool Shell::is_errexit_enabled() const {
    return get_shell_option(ShellOption::Errexit);
}

void Shell::set_errexit_severity(const std::string& severity) {
    std::string lower_severity = string_utils::to_lower_copy(severity);

    auto parsed = parse_errexit_severity_value(lower_severity);
    errexit_severity_level = parsed.value_or(ErrorSeverity::ERROR);
}

std::string Shell::get_errexit_severity() const {
    return errexit_severity_name(errexit_severity_level);
}

bool Shell::should_abort_on_nonzero_exit() const {
    if (!is_errexit_enabled()) {
        return false;
    }

    return config::is_bash_mode() || errexit_severity_level != ErrorSeverity::CRITICAL;
}

bool Shell::should_abort_on_nonzero_exit(int exit_code) const {
    if (!is_errexit_enabled()) {
        return false;
    }

    if (config::is_bash_mode()) {
        return exit_code != 0;
    }

    ErrorSeverity error_severity = ErrorSeverity::ERROR;  // default

    if (exit_code == 127) {
        error_severity = ErrorInfo::get_default_severity(ErrorType::COMMAND_NOT_FOUND);
    } else if (exit_code == 126) {
        error_severity = ErrorInfo::get_default_severity(ErrorType::PERMISSION_DENIED);
    } else if (exit_code == 2) {
        error_severity = ErrorInfo::get_default_severity(ErrorType::SYNTAX_ERROR);
    }

    return error_severity >= errexit_severity_level;
}

std::unordered_set<std::string> Shell::get_available_commands() const {
    std::unordered_set<std::string> cmds;
    if (built_ins) {
        auto b = built_ins->get_builtin_commands();
        cmds.insert(b.begin(), b.end());
    }
    for (const auto& alias : aliases) {
        (void)cmds.insert(alias.first);
    }

    if (shell_script_interpreter) {
        auto function_names = shell_script_interpreter->get_function_names();
        cmds.insert(function_names.begin(), function_names.end());
    }
    return cmds;
}

std::string Shell::get_previous_directory() const {
    return built_ins->get_previous_directory();
}

Built_ins* Shell::get_built_ins() {
    return built_ins.get();
}

ShellScriptInterpreter* Shell::get_shell_script_interpreter() {
    return shell_script_interpreter.get();
}

Parser* Shell::get_parser() {
    return shell_parser.get();
}
