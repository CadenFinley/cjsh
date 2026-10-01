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
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

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
#include "shell_env.h"
#include "signal_handler.h"
#include "trap_command.h"
#include "usage.h"
#include "version_command.h"

std::unique_ptr<Shell> g_shell = nullptr;

namespace {

void cleanup_resources() {
    // Both main and atexit use this dispatcher; shutdown must only run once.
    static bool cleanup_already_invoked = false;
    if (cleanup_already_invoked) {
        return;
    }
    cleanup_already_invoked = true;

    if (!g_shell) {
        return;
    }

    // Resolve a signal that arrived as execution returned, then keep all cleanup
    // paths in this one dispatcher. Further terminating signals cannot reenter it.
    {
        SignalMask transition({SIGHUP, SIGTERM});
        (void)g_shell->process_pending_signals();
        SignalHandler::begin_shutdown();
    }
    const int termination_signal = SignalHandler::termination_signal();
    const int status = termination_signal != 0
                           ? 128 + termination_signal
                           : numeric_utils::parse_exit_status_or(
                                 cjsh_env::get_shell_variable_value("?"), 0, false);
    g_shell->run_exit_handlers(status);

    // Destroy the shell before static teardown so its dependencies are still available.
    g_shell.reset();
    trap_manager_set_shell(nullptr);
    SignalHandler::finish_shutdown();
}

void initialize_shell(int argc, char* argv[], const flags::ParseResult& parse_result) {
    // Register before construction to cover exit() calls during initialization.
    if (std::atexit(cleanup_resources) != 0) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "",
                     "failed to set exit handler",
                     {"resource cleanup may not occur properly"}});
    }

    g_shell = std::make_unique<Shell>();
    g_shell->apply_startup_options(parse_result.shell_options);
    g_shell->set_interactive_mode(config::interactive_mode);
    for (const auto& [shopt, reusable] : parse_result.option_queries) {
        std::vector<std::string> query{shopt ? "shopt" : "set"};
        if (!shopt) {
            query.emplace_back(reusable ? "+o" : "-o");
        } else if (reusable) {
            query.emplace_back("-p");
        }
        (void)g_shell->get_built_ins()->builtin_command(query);
    }
    if (config::interactive_mode) {
        g_shell->setup_interactive_handlers();
    }

    if (!parse_result.script_args.empty()) {
        flags::set_positional_parameters(parse_result.script_args);
    }

    cjsh_env::setup_environment_variables(argv[0]);
    flags::save_startup_arguments(argc, argv);
    cjsh_env::sync_env_vars_from_system(*g_shell);

    if (const char* root = std::getenv("CJSH_CONFIG_HOME");
        root && root[0] != '\0' && config::config_directory.empty()) {
        config::config_directory = root;
    }

    if (!config::config_directory.empty()) {
        config::config_directory =
            cjsh_filesystem::normalize_override_path(config::config_directory).string();
    }

    // Startup files see the same invocation identity and arguments as the body.
    if (!parse_result.script_file.empty()) {
        (void)setenv("0", parse_result.script_file.c_str(), 1);
        (void)cjsh_env::set_shell_variable_value("0", parse_result.script_file);
    }

    if (!config::login_mode || !config::interactive_mode) {
        return;
    }

    // Keep inherited descriptors usable by builtins while preventing accidental inheritance
    // by external commands in interactive login sessions. Match Bash's 3-19 range, after
    // account lookup and before startup files can explicitly open descriptors for children.
    for (int fd = STDERR_FILENO + 1; fd < 20; ++fd) {
        (void)cjsh_filesystem::set_close_on_exec(fd);
    }
}

void process_startup_files() {
    // Environment files precede login profiles and the interactive POSIX ENV file.
    cjsh_filesystem::process_env_files();

    if (config::login_mode && !cjsh_env::exit_requested()) {
        cjsh_filesystem::process_profile_files();
    }

    if (config::is_posix_mode() && config::interactive_mode && !cjsh_env::exit_requested()) {
        cjsh_filesystem::process_posix_env_file();
    }
}

int run_command_or_script(const std::string& script_file, bool startup_interrupted = false) {
    cjsh_env::set_startup_active(false);
    if (cjsh_env::exit_requested()) {
        return read_exit_code_or(0);
    }

    if (startup_interrupted) {
        return 128 + SIGINT;
    }

    completion_history::apply_pending_history_limit();
    if (config::execute_command) {
        return read_exit_code_or(g_shell->execute(config::cmd_to_execute));
    }

    return handle_non_interactive_mode(script_file);
}

int run_interactive_session(const std::string& script_file) {
    flags::warn_if_invoked_via_sh();

    g_shell->set_interactive_mode(true);
    (void)cjsh_filesystem::initialize_cjsh_directories();

    prompt::initialize_colors();
    (void)cjsh_env::update_terminal_dimensions();

    cjsh_filesystem::process_source_files();

    if (!cjsh_env::exit_requested()) {
        cjsh_filesystem::initialize_history_storage();
    }

    cjsh_filesystem::finalize_history_path();

    // Interactive startup is independent of the input source. A supplied command or
    // script still finishes after its body, including when stdin is a terminal.
    if ((config::execute_command || !script_file.empty() || isatty(STDIN_FILENO) == 0) &&
        !cjsh_env::exit_requested()) {
        (void)g_shell->process_pending_signals();
        return run_command_or_script(script_file, SignalHandler::startup_interrupted());
    }

    if (!cjsh_env::exit_requested() && (config::interactive_mode || config::force_interactive)) {
        start_interactive_process();
    }

    return read_exit_code_or(0);
}

int run_cjsh(int argc, char* argv[]) {
    startup_begin_time() = std::chrono::steady_clock::now();
    cjsh_env::reset_shell_state();

    const flags::ParseResult parse_result = flags::parse_arguments(argc, argv);
    if (parse_result.should_exit) {
        return parse_result.exit_code;
    }

    if (config::show_version) {
        return version_command({});
    }

    if (config::show_help) {
        return print_usage();
    }

    // CJSH does not support retaining set-user-ID or set-group-ID privileges.
    // Refuse before constructing subsystems or consulting any startup paths.
    if (getuid() != geteuid() || getgid() != getegid()) {
        print_error({ErrorType::RUNTIME_ERROR,
                     "startup",
                     "refusing startup with mismatched real and effective IDs",
                     {}});
        return 1;
    }

    initialize_shell(argc, argv, parse_result);
    process_startup_files();

    if (cjsh_env::exit_requested()) {
        return read_exit_code_or(0);
    }

    if (config::interactive_mode) {
        return run_interactive_session(parse_result.script_file);
    }

    cjsh_filesystem::finalize_history_path();
    return run_command_or_script(parse_result.script_file);
}

}  // namespace

int main(int argc, char* argv[]) {
    // main entry
    // we split off the main cjsh runner to allow atexit() to properly scope cleanup if cjsh has to
    // exit through a non normal path
    const int exit_code = run_cjsh(argc, argv);

    // Normal returns share cleanup with exit(), after publishing the command status.
    pipeline_status_utils::set_last_status_env(exit_code);
    cleanup_resources();
    return read_exit_code_or(exit_code);
}
