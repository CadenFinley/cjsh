/*
  cjsh.cpp

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

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "completion_history.h"
#include "error_out.h"
#include "flags.h"
#include "main_loop.h"
#include "numeric_utils.h"
#include "pipeline_status_utils.h"
#include "prompt.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "trap_command.h"
#include "usage.h"
#include "version_command.h"

// coordinate the process lifetime here: parse the invocation, prepare the shell,
// source the applicable startup files, select an input path, and run cleanup.
// the shell is shared with builtins and execution helpers, but this file owns its
// lifetime so exit hooks finish before its subsystems are destroyed.
std::unique_ptr<Shell> shell = nullptr;

namespace {

// cleanup can change the final status through exit hooks. leave this unset until
// a live shell has run those hooks, so early exits can retain their own status.
std::optional<int> cleanup_exit_status;

// funnel normal returns and explicit exit() calls through the same shutdown
// sequence while the shell, trap manager, and other static dependencies exist.
void cleanup_resources() {
    // main calls this directly, and atexit may call it again afterward. set the
    // guard before invoking any shell code so cleanup cannot recursively run hooks.
    static bool cleanup_already_invoked = false;
    if (cleanup_already_invoked) {
        return;
    }
    cleanup_already_invoked = true;

    // help, version output, and argument errors can finish without constructing
    // a shell. an exit during construction can also reach this callback too early.
    if (!shell) {
        return;
    }

    // block hangup and termination signals during the handoff to shutdown. first
    // dispatch any pending signals while the shell is intact, then mark shutdown
    // active before restoring the mask. later terminating signals can be recorded
    // without starting another cleanup sequence while exit hooks are running.
    {
        SignalMask transition({SIGHUP, SIGTERM});
        (void)shell->process_pending_signals();
        SignalHandler::begin_shutdown();
    }

    // exit handlers receive the terminating signal's conventional status when
    // present; otherwise use the last command status, with zero as the fallback.
    const int termination_signal = SignalHandler::termination_signal();
    const int status = termination_signal != 0
                           ? 128 + termination_signal
                           : numeric_utils::parse_exit_status_or(
                                 cjsh_env::get_shell_variable_value("?"), 0, false);

    // the shell coordinates its exit function, exit trap, and login logout file.
    // capture any explicit exit status they leave before destroying shell state.
    shell->run_exit_handlers(status);
    cleanup_exit_status = read_exit_code_or(status);

    // destruction handles remaining jobs and restores the terminal. detach the
    // trap manager afterward so it cannot retain a pointer to the destroyed shell.
    shell.reset();
    trap_manager_set_shell(nullptr);

    // restore termination handling only after resources are released. if a signal
    // caused shutdown, this can re-raise it so the parent observes signal death.
    SignalHandler::finish_shutdown();
}

// establish the options, arguments, environment, and descriptor policy before
// executing any startup files or user commands.
void initialize_shell(int argc, char* argv[], const flags::ParseResult& parse_result) {
    // register before construction so explicit exit() calls during initialization
    // reach cleanup. failure is reported, but ordinary returns still clean up in main.
    if (std::atexit(cleanup_resources) != 0) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "",
                     "failed to set exit handler",
                     {"resource cleanup may not occur properly"}});
    }

    // construct the execution subsystems, then apply invocation options before
    // queries or startup files inspect them. interactivity also affects defaults.
    shell = std::make_unique<Shell>();
    shell->apply_startup_options(parse_result.shell_options);
    shell->set_interactive_mode(config::interactive_mode);

    // reuse the set and shopt builtins to display requested option listings. the
    // reusable form prints commands that can restore the current option settings.
    for (const auto& [shopt, reusable] : parse_result.option_queries) {
        std::vector<std::string> query{shopt ? "shopt" : "set"};
        if (!shopt) {
            query.emplace_back(reusable ? "+o" : "-o");
        } else if (reusable) {
            query.emplace_back("-p");
        }
        (void)shell->get_builtins()->builtin_command(query);
    }

    // install interactive signal dispositions before startup files run, including
    // terminal resize handling and protection from terminal quit and stop signals.
    if (config::interactive_mode) {
        shell->setup_interactive_handlers();
    }

    // positional arguments must already be available to expansions in startup files.
    if (!parse_result.script_args.empty()) {
        flags::set_positional_parameters(parse_result.script_args);
    }

    // bootstrap process variables from the invocation and account information,
    // retain the original argument vector for restart helpers, then import the
    // resulting environment into the shell and parser.
    cjsh_env::setup_environment_variables(argv[0]);
    flags::save_startup_arguments(argc, argv);
    cjsh_env::sync_env_vars_from_system(*shell);

    // an explicit configuration directory from the command line takes precedence
    // over the environment override. an empty override leaves the default in place.
    if (const char* root = std::getenv("CJSH_CONFIG_HOME");
        root && root[0] != '\0' && config::config_directory.empty()) {
        config::config_directory = root;
    }

    // expand a leading tilde and resolve relative paths before startup files can
    // change the working directory used to interpret the override.
    if (!config::config_directory.empty()) {
        config::config_directory =
            cjsh_filesystem::normalize_override_path(config::config_directory).string();
    }

    // use the script path, or the explicit command name supplied after -c, as $0.
    // update both variable stores so startup files and the body see the same identity.
    if (!parse_result.script_file.empty()) {
        (void)setenv("0", parse_result.script_file.c_str(), 1);
        (void)cjsh_env::set_shell_variable_value("0", parse_result.script_file);
    }

    // descriptor inheritance is restricted here only for interactive login shells.
    // other invocation modes keep the descriptor flags supplied by their caller.
    if (!config::login_mode || !config::interactive_mode) {
        return;
    }

    // keep inherited descriptors usable by builtins while preventing accidental inheritance
    // by external commands in interactive login sessions. close-on-exec leaves the
    // descriptors open in this process. match bash's 3-19 range, after account lookup
    // and before startup files can explicitly open descriptors for children.
    for (int fd = STDERR_FILENO + 1; fd < 20; ++fd) {
        (void)cjsh_filesystem::set_close_on_exec(fd);
    }
}

// source the startup stages shared by command, script, and interactive invocations.
// the filesystem helpers enforce dialect rules, disabled configuration, and startup
// interruption; this dispatcher preserves ordering and stops after an exit request.
void process_startup_files() {
    // environment setup comes first so later profiles can use the variables it sets.
    cjsh_filesystem::process_env_files();

    // login profiles run for login invocations regardless of whether input will
    // eventually come from a prompt, a command string, or a script.
    if (config::login_mode && !cjsh_env::exit_requested()) {
        cjsh_filesystem::process_profile_files();
    }

    // the posix environment startup file belongs only to interactive shells and
    // follows any login profiles, which may have changed its configured location.
    if (config::is_posix_mode() && config::interactive_mode && !cjsh_env::exit_requested()) {
        cjsh_filesystem::process_posix_env_file();
    }
}

// execute a command string, script, or standard input after startup has finished.
// the caller snapshots startup interruption because clearing startup state below
// also makes the signal handler's startup-specific interruption query inactive.
int run_command_or_script(const std::string& script_file, bool startup_interrupted = false) {
    cjsh_env::set_startup_active(false);

    // an exit requested by startup code takes precedence over cancellation and must
    // prevent the requested command or script from starting.
    if (cjsh_env::exit_requested()) {
        return read_exit_code_or(0);
    }

    // an interrupted interactive startup returns the conventional interrupt status
    // without starting the requested body.
    if (startup_interrupted) {
        return 128 + SIGINT;
    }

    // these paths do not initialize the prompt's line editor, so apply history
    // limits deferred by startup code here after its path and settings are settled.
    completion_history::apply_pending_history_limit();

    // command strings use the interpreter directly. an explicit exit requested by
    // the command overrides its ordinary execution result when one is available.
    if (config::execute_command) {
        return read_exit_code_or(shell->execute(config::cmd_to_execute));
    }

    // the shared reader loads a named script or reads standard input when no file
    // was supplied; it also owns input errors and the resulting execution status.
    return handle_non_interactive_mode(script_file);
}

// perform interactive setup even when -i accompanies a command string, script, or
// redirected input. entering the prompt loop is a separate decision made afterward.
int run_interactive_session(const std::string& script_file) {
    // limit the sh compatibility warning to invocations using interactive behavior.
    flags::warn_if_invoked_via_sh();

    // prepare persistence and terminal presentation before the interactive startup
    // file runs. directory setup can disable unavailable persistence and still continue.
    shell->set_interactive_mode(true);
    (void)cjsh_filesystem::initialize_cjsh_directories();

    prompt::initialize_colors();
    (void)cjsh_env::update_terminal_dimensions();

    // interactive configuration follows the common environment and profile stages.
    // its helper skips cjsh-specific source files when the selected dialect requires it.
    cjsh_filesystem::process_source_files();

    // wait until interactive configuration has chosen the history path and enabled
    // features before creating storage. an exit from that configuration skips creation.
    if (!cjsh_env::exit_requested()) {
        cjsh_filesystem::initialize_history_storage();
    }

    // refresh any earlier history path lookup and freeze the startup selection so
    // later history operations use the same destination throughout the session.
    cjsh_filesystem::finalize_history_path();

    // interactive startup is independent of the input source. a supplied command or
    // script still finishes after its body, including when stdin is a terminal.
    if ((config::execute_command || !script_file.empty() || isatty(STDIN_FILENO) == 0) &&
        !cjsh_env::exit_requested()) {
        // dispatch signals once more before sampling startup cancellation. pass the
        // result by value because the execution helper ends the startup phase.
        (void)shell->process_pending_signals();
        return run_command_or_script(script_file, SignalHandler::startup_interrupted());
    }

    // when no command or script was supplied and input is a terminal, the main loop
    // initializes the line editor, ends startup, and reads until exit or end of input.
    if (!cjsh_env::exit_requested() && (config::interactive_mode || config::force_interactive)) {
        start_interactive_process();
    }

    // preserve an explicit exit from startup or the prompt loop, defaulting to success.
    return read_exit_code_or(0);
}

// run initialization and the selected input path, leaving final cleanup to main or
// the registered exit callback. returning here lets invocation-local objects unwind
// before normal shutdown begins.
int run_cjsh(int argc, char* argv[]) {
    // include parsing and configuration in the startup duration shown by the prompt.
    // reset exit tracking and mark startup active before any shell code can run.
    startup_begin_time() = std::chrono::steady_clock::now();
    cjsh_env::reset_shell_state();

    // parsing selects the dialect and execution mode as well as the script and
    // arguments. errors return immediately without allocating shell subsystems.
    const flags::ParseResult parse_result = flags::parse_arguments(argc, argv);
    if (parse_result.should_exit) {
        return parse_result.exit_code;
    }

    // informational requests need neither a live shell nor user startup files.
    if (config::show_version) {
        return version_command({});
    }

    if (config::show_help) {
        return print_usage();
    }

    // cjsh does not support retaining set-user-id or set-group-id privileges.
    // refuse before constructing subsystems or consulting any startup paths.
    if (getuid() != geteuid() || getgid() != getegid()) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "startup",
                     "refusing startup with mismatched real and effective IDs",
                     {}});
        return 1;
    }

    // adopt the environment's locale before constructing the shell so startup files
    // and later commands use its character classification and formatting rules.
    (void)std::setlocale(LC_ALL, "");
    initialize_shell(argc, argv, parse_result);
    process_startup_files();

    // common startup files may terminate the invocation before interactive setup
    // or the requested body. propagate that status to the shared cleanup path.
    if (cjsh_env::exit_requested()) {
        return read_exit_code_or(0);
    }

    // interactive mode controls additional startup work, even if its eventual
    // execution path is a command string, script, or redirected standard input.
    if (config::interactive_mode) {
        return run_interactive_session(parse_result.script_file);
    }

    // non-interactive startup has no later source-file stage. settle history now,
    // while leaving history paths lazy for scripts that never use them.
    cjsh_filesystem::finalize_history_path();
    return run_command_or_script(parse_result.script_file);
}

}  // namespace

int main(int argc, char* argv[]) {
    // keep the invocation's local state inside the runner so it unwinds before
    // normal cleanup. explicit exit() calls reach the registered callback instead.
    const int exit_code = run_cjsh(argc, argv);

    // publish the result as $? before exit hooks inspect it. cleanup may supply a
    // replacement status, while early returns without a shell retain the runner's
    // result. the later atexit callback is harmless because cleanup is guarded.
    pipeline_status_utils::set_last_status_env(exit_code);
    cleanup_resources();
    return cleanup_exit_status.value_or(exit_code);
}
