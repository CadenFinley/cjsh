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
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "trap_command.h"

// own and connect the execution subsystems, dispatch commands, and manage the
// shell's terminal and option state. cjsh.cpp owns the process lifetime; this
// layer is shared by startup files, the interactive loop, and script evaluation.
namespace {

constexpr size_t to_index(ShellOption option) {
    return static_cast<size_t>(option);
}

// keep invocation parsing and builtin option listings on the same names. shopt
// entries use a separate lookup path from the options accepted by set -o.
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
                                {ShellOption::BraceExpand, 'B', "braceexpand"},
                                {ShellOption::HistExpand, 'H', "histexpand"},
                                {ShellOption::Autocd, 0, "autocd", true}}};

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

// build the subsystem graph before attaching process-wide managers or installing
// signal dispositions that depend on terminal ownership.
Shell::Shell() : shell_pid(getpid()) {
    trap_manager_initialize();

    // the shell owns these objects; their cross-references below are non-owning.
    executor = std::make_unique<Exec>();
    signal_handler = std::make_unique<SignalHandler>();
    parser = std::make_unique<Parser>();
    builtins = std::make_unique<Built_ins>();
    interpreter = std::make_unique<ShellScriptInterpreter>();

    // let parsing, evaluation, and builtins share this shell's state rather than
    // creating independent variable, option, or directory contexts.
    if (interpreter && parser) {
        interpreter->set_parser(parser.get());
        parser->set_shell(this);
    }
    builtins->set_shell(this);
    builtins->set_current_directory();

    // start with stdin; job-control setup prefers a private terminal descriptor
    // so later command redirections do not change the terminal used for handoff.
    shell_terminal = STDIN_FILENO;

    // job and trap managers need every subsystem initialized before they attach to the shell
    JobManager::instance().set_shell(this);
    trap_manager_set_shell(this);

    // a nested interactive shell must let the kernel stop it with SIGTTIN until
    // its parent foregrounds it. install ignored job-control dispositions only
    // after this handshake, or the shell could spin instead of stopping.
    setup_job_control();

    // signal dispatch depends on the prior wiring so register handlers after setup completes
    setup_signal_handlers();
}

Shell::~Shell() {
    // exit hooks run before destruction. now either signal remaining children
    // or detach them according to the shutdown cause and huponexit setting.
    if (executor) {
        const int terminating_signal = SignalHandler::termination_signal();
        const bool hang_up_on_exit = get_shell_option(ShellOption::Huponexit);
        if (terminating_signal != 0 || hang_up_on_exit) {
            executor->terminate_all_child_process(terminating_signal == SIGTERM ? SIGTERM : SIGHUP);
        } else {
            executor->abandon_all_child_processes();
        }
    }

    // discard job records only after execution-side child cleanup has used them.
    JobManager::instance().clear_all_jobs();

    // restore the terminal modes captured at startup, then close only a descriptor
    // owned by this shell. inherited stdin must remain open for other cleanup.
    restore_terminal_state();
    if (owns_shell_terminal) {
        (void)close(shell_terminal);
    }

    // -i alone does not imply a prompt session. suppress the farewell unless
    // interactive input actually began and startup has ended.
    if (interactive_input_started && !cjsh_env::startup_active()) {
        if (config::login_mode) {
            std::cout << "cjsh logout";
        } else {
            std::cout << "cjsh exit";
        }
        (void)std::cout.flush();
    }
}

// run shutdown code while the interpreter and terminal are still available.
// preserve the order: native exit function, EXIT trap, then login logout file.
void Shell::run_exit_handlers(int status) {
    // an exit hook can launch a subshell. its inherited guard must prevent that
    // subshell from recursively invoking the same shutdown sequence.
    if (exit_handlers_invoked) {
        return;
    }
    exit_handlers_invoked = true;

    // each stage sees the original status, not the previous hook's return value.
    // clear the exit request so one stage cannot prevent the next from executing.
    const auto prepare_handler = [status] {
        cjsh_env::clear_exit_request();
        pipeline_status_utils::set_last_status_env(status);
    };
    prepare_handler();
    trap_manager_set_shell(this);

    if (interpreter != nullptr && !config::minimal_mode && !config::secure_mode &&
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

// evaluate shell text in the current shell. the interpreter owns control flow
// and expansion; only commands that need simple dispatch come back through the
// command entry points below.
int Shell::execute(const std::string& script, bool skip_validation) {
    mark_terminal_dirty();
    if (script.empty()) {
        return 0;
    }

    // preserve shell-aware line boundaries rather than splitting blindly on newlines.
    std::vector<std::string> lines = parser->parse_into_lines(script);

    if (interpreter) {
        // remember the submitted text after evaluation so nested execution cannot
        // leave last_command pointing at an inner command instead.
        int exit_code = interpreter->execute_block(lines, skip_validation, false);
        last_command = script;
        return exit_code;
    }
    print_error(ErrorInfo{ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    return 1;
}

// separate leading assignments once, then share dispatch with callers that
// already prepared the command during interpretation.
int Shell::execute_command(std::vector<std::string> args, bool run_in_background,
                           bool auto_background_on_stop, bool auto_background_on_stop_silent) {
    return execute_prepared_command(cjsh_env::prepare_command(std::move(args)), run_in_background,
                                    auto_background_on_stop, auto_background_on_stop_silent);
}

int Shell::execute_prepared_command(cjsh_env::PreparedCommand command, bool run_in_background,
                                    bool auto_background_on_stop,
                                    bool auto_background_on_stop_silent) {
    const auto& args = command.original_args;
    // reject readonly assignment prefixes before executing any part of the command.
    // the POSIX error helper also requests exit for non-interactive shells.
    if (config::is_posix_mode()) {
        for (const auto& [name, value] : command.assignments) {
            if (!readonly_manager_can_assign(name, "assignment")) {
                return cjsh_env::posix_error_exit(1);
            }
        }
    }
    // dispatch requires both the builtin and external execution paths to be wired.
    if (!executor || !builtins) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }

    mark_terminal_dirty();
    if (args.empty()) {
        return 0;
    }

    // trace the original words, including assignment prefixes, before noexec can
    // suppress dispatch. the trace prompt is expanded at execution time.
    if (get_shell_option(ShellOption::Xtrace)) {
        std::cerr << prompt::render_trace_prompt() << string_utils::join_strings(args, " ") << '\n';
    }

    if (get_shell_option(ShellOption::Noexec)) {
        return 0;
    }

    // a lone assignment changes shell state instead of launching a process.
    // expand its value through the parser and store it through variable management.
    if (args.size() == 1 && parser) {
        std::string var_name;
        std::string var_value;
        if (parser->is_env_assignment(args[0], var_name, var_value)) {
            parser->expand_env_vars(var_value);

            if (interpreter) {
                interpreter->get_variable_manager().set_environment_variable(var_name, var_value);
            }

            return 0;
        }

        if (args[0].find('=') != std::string::npos) {
            return 1;
        }
    }

    // prepared arguments exclude assignment prefixes. foreground POSIX special
    // builtins retain those assignments; other direct commands restore them on return.
    const auto& env_assignments = command.assignments;
    const auto& command_args = command.args;
    const bool has_temporary_env = !env_assignments.empty() && !command_args.empty();
    const bool assignments_persist =
        has_temporary_env && !run_in_background && is_posix_special_builtin(command_args[0]);

    const bool is_direct_command =
        !command_args.empty() && (builtins->is_builtin_or_runtime_command(command_args[0]) != 0);

    command.is_builtin = is_direct_command;

    // builtins and runtime commands execute in the current process context, which
    // may itself be a subshell. scope their environment changes around dispatch.
    if (is_direct_command) {
        cjsh_env::TemporaryEnvAssignmentScope assignments(this, env_assignments,
                                                          assignments_persist);
        return builtins->builtin_or_runtime_command(command_args);
    }

    // implicit cd is only considered for a lone foreground interactive token
    // after builtin lookup; the lookup helper enforces autocd and dialect policy.
    if (interactive_mode && !run_in_background && command_args.size() == 1 && builtins) {
        const std::string& candidate = command_args[0];

        if (command_lookup::should_auto_cd_token(candidate, this)) {
            std::vector<std::string> cd_args = {"cd", candidate};
            int code = builtins->builtin_command(cd_args);
            return code;
        }
    }

    // background launch returns without waiting. publish the job's last process
    // as $! when launch produced a job record; its completion is handled later.
    if (run_in_background) {
        int job_id = executor->execute_prepared_command_async(std::move(command));
        if (job_id > 0) {
            auto jobs = executor->get_jobs();
            auto it = jobs.find(job_id);
            if (it != jobs.end() && !it->second.pids.empty()) {
                pid_t last_pid = it->second.pids.back();
                (void)setenv("!", std::to_string(last_pid).c_str(), 1);

                JobManager::instance().set_last_background_pid(last_pid);
            }
        }
        return 0;
    }

    // let the execution layer own foreground waiting and stopped-job handling,
    // then report any launch error associated with its result.
    int exit_code = executor->execute_prepared_command_sync(
        std::move(command), auto_background_on_stop, auto_background_on_stop_silent);
    executor->print_error_if_needed(exit_code);
    return exit_code;
}

// read a source file into the current shell rather than starting a new process.
// optional files suppress open failures, but still report errors in loaded code.
int Shell::execute_script_file(const std::filesystem::path& path, bool optional) {
    if (!interpreter) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }

    // settle the diagnostic path before the script can change directories. lexical
    // normalization does not require resolving symlinks or an existing target.
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
    if (!interpreter) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }
    auto parsed_lines = interpreter->parse_into_lines(content);
    if (parsed_lines.empty()) {
        return 0;
    }

    // nested source calls temporarily replace diagnostic context and establish a
    // return boundary without discarding the surrounding shell's state.
    const std::string previous_error_source = interpreter->get_error_source();
    interpreter->set_error_source(source_path);
    interpreter->push_source_scope();
    int exit_code = interpreter->execute_block(parsed_lines, false, false);
    interpreter->pop_source_scope();
    interpreter->set_error_source(previous_error_source);

    // A source boundary consumes only an explicit return request.
    if (auto returned = interpreter->control_flow_state().consume_return()) {
        exit_code = *returned;
        pipeline_status_utils::set_last_status_env(exit_code);
    }
    return exit_code;
}

// consume a nonempty explicit exit status once. callers use their execution
// result when no override is present or its value cannot be parsed.
int read_exit_code_or(int fallback) {
    std::string exit_code_str = cjsh_env::get_shell_variable_value("EXIT_CODE");
    if (exit_code_str.empty()) {
        return fallback;
    }

    fallback = numeric_utils::parse_exit_status_or(exit_code_str, fallback, false);
    (void)cjsh_env::unset_shell_variable_value("EXIT_CODE");
    return fallback;
}

// dispatch recorded signals from normal execution, not from a signal handler.
// traps and child notifications may disturb the terminal, so invalidate recovery
// before handing control to the signal subsystem.
SignalProcessingResult Shell::process_pending_signals(bool reap_children) {
    if (!signal_handler) {
        return {};
    }

    if (!SignalHandler::has_pending_signals()) {
        return {};
    }

    mark_terminal_dirty();
    return signal_handler->process_pending_signals(executor.get(), reap_children);
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

// separate process-group policy from terminal capability. monitor mode can be
// useful without a tty, but foreground handoff requires a successful handshake.
void Shell::setup_job_control() {
    const bool requested_interactive = config::interactive_mode || config::force_interactive;
    if (!requested_interactive) {
        job_control_enabled = false;
        shell_options[to_index(ShellOption::Monitor)] = false;
        interactive_job_control_available = false;
        return;
    }
    // stdio may point at a different pty from /dev/tty. prefer that terminal and
    // keep a private close-on-exec descriptor across builtin redirections.
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
        // forced interactive execution without a terminal still has job groups.
        job_control_enabled = true;
        shell_options[to_index(ShellOption::Monitor)] = true;
        interactive_job_control_available = false;
        return;
    }

    shell_pgid = getpgrp();

    // a background shell waits for its parent to foreground the process group.
    // restore SIGTTIN first because ignored dispositions survive exec. login
    // launchers may instead expect this shell to claim the terminal itself;
    // stopping in that case could deadlock with a launcher waiting for shell exit.
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
        // orphaned process groups discard SIGTTIN. bound repeated attempts without
        // imposing a timeout on a shell stopped normally until fg.
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

    // a session leader already leads its process group and setpgid reports EPERM.
    // accept that failure only when the desired group is actually in place.
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

    // claiming the terminal can itself raise SIGTTOU while we are in the background.
    // block it during the handoff and preserve the caller's signal mask afterward.
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

// terminal ownership belongs to the original foreground shell, not to forked
// copies of this object or to any process that merely has monitor mode enabled.
bool Shell::manages_terminal() const {
    return interactive_job_control_available && shell_pgid > 0 && getpid() == shell_pgid &&
           getpgrp() == shell_pgid;
}

// avoid repeated editor recovery when nothing ran between prompt operations.
// clear first so a concurrent invalidation remains visible; retry failed handoff
// on the next call rather than treating an unrecovered terminal as clean.
void Shell::recover_prompt_terminal() {
    if (!prompt_terminal_dirty.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    if (!reclaim_terminal()) {
        mark_terminal_dirty();
        return;
    }
    ic_recover_terminal();
}

bool Shell::reclaim_terminal() const {
    // forked subshells must not take the terminal from their parent. conversely,
    // the original shell still needs recovery after the user disables monitor mode.
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

    // a widget can suspend the shell from inside readline. expose ordinary terminal
    // modes while the parent shell handles the stop notification.
    const bool editor_active = ic_suspend_readline_terminal();
    ic_prepare_terminal_for_command();
    (void)fflush(stdout);
    (void)fflush(stderr);
    const bool stopped = kill(getpid(), SIGSTOP) == 0;

    // a background continuation must wait for fg instead of stealing the tty.
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
    // monitor mode controls process-group creation even without terminal handoff.
    // an interactive shell with a tty may enable it only after startup established
    // that the shell can manage that terminal.
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
    if (parser) {
        parser->set_aliases(aliases);
    }
}

std::unordered_map<std::string, std::string>& Shell::get_aliases() {
    return aliases;
}

std::vector<std::string>& Shell::get_directory_stack() {
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

// apply options in invocation order so repeated settings have last-option-wins
// behavior and use the same side effects as later set or shopt commands.
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

// remember explicit choices separately from defaults. some options also live in
// shared configuration or require terminal checks instead of a plain flag update.
void Shell::set_shell_option(ShellOption option, bool value) {
    explicit_shell_options[to_index(option)] = true;
    if (option == ShellOption::Noexec && config::interactive_mode) {
        if (value) {
            print_error({ErrorType::UNKNOWN_ERROR,
                         ErrorSeverity::WARNING,
                         "warning",
                         "noexec is ignored in interactive mode",
                         {"Use cjsh -n script to check syntax without executing commands."}});
        }
        value = false;
        // An ignored startup -n must not suppress startup files or PATH setup.
        config::no_exec = false;
    }
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

// resolve effective values at lookup time: dialect restrictions take precedence,
// and untouched options can depend on the current dialect or invocation mode.
bool Shell::get_shell_option(ShellOption option) const {
    if (option == ShellOption::Extglob) {
        return config::extglob_enabled;
    }
    if (option == ShellOption::HistExpand) {
        return config::history_expansion_enabled;
    }
    if (config::is_posix_mode() &&
        (option == ShellOption::Globstar || option == ShellOption::BraceExpand ||
         option == ShellOption::Autocd)) {
        return false;
    }
    if (!explicit_shell_options[to_index(option)]) {
        switch (option) {
            case ShellOption::Autocd:
                return !config::is_posix_mode();
            case ShellOption::ExpandAliases:
                return true;
            case ShellOption::Huponexit:
                return config::interactive_mode;
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

// conditional evaluation can temporarily suppress errexit without changing the
// user's option. without a specific status, treat failure as ordinary error severity.
bool Shell::should_abort_on_nonzero_exit() const {
    if (!is_errexit_enabled() || errexit_suppression_depth != 0) {
        return false;
    }

    return errexit_severity_level != ErrorSeverity::CRITICAL;
}

// callers supply a failing status. map conventional launch and syntax failures
// to their diagnostic severity before applying the configured errexit threshold.
bool Shell::should_abort_on_nonzero_exit(int exit_code) const {
    if (!is_errexit_enabled() || errexit_suppression_depth != 0) {
        return false;
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

// collect shell-defined command names only. external executables require PATH
// lookup and are intentionally outside this inventory.
std::unordered_set<std::string> Shell::get_available_commands() const {
    std::unordered_set<std::string> cmds;
    if (builtins) {
        auto b = builtins->get_builtin_commands();
        cmds.insert(b.begin(), b.end());
    }
    for (const auto& alias : aliases) {
        (void)cmds.insert(alias.first);
    }

    if (interpreter) {
        auto function_names = interpreter->get_function_names();
        cmds.insert(function_names.begin(), function_names.end());
    }
    return cmds;
}

std::string Shell::get_previous_directory() const {
    return builtins->get_previous_directory();
}

Built_ins* Shell::get_builtins() {
    return builtins.get();
}

ShellScriptInterpreter* Shell::get_interpreter() {
    return interpreter.get();
}

Parser* Shell::get_parser() {
    return parser.get();
}
