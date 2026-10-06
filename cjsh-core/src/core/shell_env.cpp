/*
  shell_env.cpp

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

#include "shell_env.h"
#include <sys/types.h>

#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <unordered_map>
#include <utility>
#include "shell_dialect.h"
#if defined(__APPLE__)
#include <crt_externs.h>
#endif

#if !defined(__APPLE__)
extern "C" char** environ;
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_line_utils.h"
#include "error_out.h"
#include "interpreter.h"
#include "interpreter_utils.h"
#include "numeric_utils.h"
#include "parser_utils.h"
#include "pipeline_status_utils.h"
#include "prompt.h"
#include "shell.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "version_command.h"

namespace config {
bool login_mode = false;
bool interactive_mode = true;
bool force_interactive = false;
bool execute_command = false;
bool read_stdin = false;
std::string cmd_to_execute;
bool no_exec = false;
bool no_config = false;
bool no_system_paths = false;
std::string config_directory;
bool cache_persistence_enabled = true;
bool history_persistence_enabled = true;
bool colors_enabled = true;
bool source_enabled = true;
bool completions_enabled = true;
bool completion_learning_enabled = true;
bool smart_cd_enabled = true;
bool extglob_enabled = false;
bool syntax_highlighting_enabled = true;
bool show_version = false;
bool show_help = false;
bool minimal_mode = false;
bool show_startup_time = false;
bool secure_mode = false;
bool show_title_line = true;
bool history_enabled = true;
bool history_expansion_enabled = true;
bool newline_after_execution = false;
bool suppress_sh_warning = false;
bool status_line_enabled = true;
bool status_reporting_enabled = true;
bool script_extension_interpreter_enabled = true;
bool error_suggestions_enabled = true;
bool prompt_vars_enabled = true;
long idle_timeout_seconds = 0;
ExitConfirmationMode exit_confirmation_mode = ExitConfirmationMode::Smart;
}  // namespace config

namespace cjsh_env {

namespace {

std::unordered_set<std::string> exported_names;

bool is_process_mirrored_shell_var(const std::string& name) {
    return name == "PATH" || name == "PWD" || name == "HOME" || name == "USER" || name == "SHELL";
}

#if defined(__APPLE__)
inline char** cjsh_environ() {
    return *_NSGetEnviron();
}
#else
inline char** cjsh_environ() {
    return ::environ;
}
#endif

std::unordered_map<std::string, std::string> g_env_vars;
bool g_exit_flag = false;
volatile sig_atomic_t g_startup_active = 1;
std::uint64_t g_command_sequence = 0;

void apply_env_vars_to_parser(Shell* shell) {
    if (shell == nullptr) {
        return;
    }
    if (auto* parser = shell->get_parser()) {
        parser->set_env_vars(g_env_vars);
    }
}

}  // namespace

void setup_environment_variables(const char* argv0) {
    setup_path_variables();

    std::string shell_value = "cjsh";
    std::string existing_shell_value;
    // Raw getenv here: bootstrap from process env before shell vars exist.
    if (const char* existing_shell_env = getenv("SHELL");
        existing_shell_env != nullptr && existing_shell_env[0] != '\0') {
        existing_shell_value = existing_shell_env;
        shell_value = existing_shell_value;
    }

    std::string candidate_shell;

    if (argv0 != nullptr) {
        (void)setenv("0", argv0, 1);
        std::string argv0_str(argv0);
        if (!argv0_str.empty() && argv0_str.front() == '-') {
            (void)argv0_str.erase(argv0_str.begin());
        }
        if (!argv0_str.empty()) {
            if (argv0_str.find('/') != std::string::npos) {
                std::filesystem::path path_candidate(argv0_str);
                if (!path_candidate.is_absolute()) {
                    path_candidate = std::filesystem::absolute(path_candidate);
                }
                candidate_shell = path_candidate.lexically_normal().string();
            } else {
                std::string resolved_shell = cjsh_filesystem::find_executable_in_path(argv0_str);
                if (!resolved_shell.empty()) {
                    candidate_shell = resolved_shell;
                } else {
                    candidate_shell = argv0_str;
                }
            }
        }
    } else {
        (void)setenv("0", "cjsh", 1);
    }

    if (candidate_shell.empty() && !existing_shell_value.empty() &&
        existing_shell_value.front() == '-') {
        candidate_shell = existing_shell_value.substr(1);
    }

    if (!candidate_shell.empty()) {
        bool existing_invalid = existing_shell_value.empty() ||
                                existing_shell_value.front() == '-' || shell_value == "cjsh";
        if (existing_invalid) {
            shell_value = candidate_shell;
        }
    }

    if (!shell_value.empty() && shell_value.front() == '-') {
        (void)shell_value.erase(shell_value.begin());
    }
    if (shell_value.empty()) {
        shell_value = "cjsh";
    }

    (void)setenv("SHELL", shell_value.c_str(), 1);
    (void)setenv("_", shell_value.c_str(), 1);

    // Account lookup only supplies missing identity fields. Inherited empty user
    // labels are intentional, while an empty HOME still needs the account default.
    const char* inherited_home = getenv("HOME");
    const bool needs_account = getenv("USER") == nullptr || getenv("LOGNAME") == nullptr ||
                               inherited_home == nullptr || inherited_home[0] == '\0';
    const struct passwd* pw = needs_account ? getpwuid(getuid()) : nullptr;

    if (!needs_account || pw != nullptr) {
        auto env_vars = setup_user_system_vars(
            pw, g_shell ? g_shell->get_built_ins()->get_current_directory() : std::string{});

        for (const auto& [name, value] : env_vars) {
            (void)setenv(name.c_str(), value.c_str(), 1);
        }
    }
}

std::string get_shell_variable_value(const std::string& name) {
    if (g_shell) {
        if (auto* interpreter = g_shell->get_shell_script_interpreter()) {
            return interpreter->get_variable_value(name);
        }
    }
    // Raw getenv fallback: interpreter not available yet.
    const char* value = getenv(name.c_str());
    return value != nullptr ? value : "";
}

std::string get_shell_variable_value(const char* name) {
    if (name == nullptr) {
        return {};
    }
    return get_shell_variable_value(std::string(name));
}

bool shell_variable_is_set(const std::string& name) {
    if (g_shell) {
        if (auto* interpreter = g_shell->get_shell_script_interpreter()) {
            return interpreter->get_variable_manager().variable_is_set(name);
        }
    }
    // Raw getenv fallback: interpreter not available yet.
    return getenv(name.c_str()) != nullptr;
}

bool shell_variable_is_set(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return shell_variable_is_set(std::string(name));
}

bool set_shell_variable_value(const std::string& name, const std::string& value) {
    if (!g_shell) {
        return false;
    }
    auto* interpreter = g_shell->get_shell_script_interpreter();
    if (!interpreter) {
        return false;
    }
    interpreter->get_variable_manager().set_environment_variable(name, value);
    return true;
}

bool unset_shell_variable_value(const std::string& name) {
    if (!g_shell) {
        return false;
    }
    auto* interpreter = g_shell->get_shell_script_interpreter();
    if (!interpreter) {
        return false;
    }
    auto& env_map = env_vars();
    size_t erased = env_map.erase(name);
    if (erased == 0 && !should_mirror_to_process_env(name)) {
        return true;
    }

    if (auto* parser = g_shell->get_parser()) {
        parser->unset_env_var(name);
    }

    mirror_unset_from_process_env(name);
    return true;
}

bool should_mirror_to_process_env(const std::string& name) {
    return config::is_posix_mode() || is_process_mirrored_shell_var(name);
}

void mark_exported(const std::string& name) {
    exported_names.insert(name);
}

std::vector<std::string> exported_variable_names() {
    auto names = exported_names;
    for (char** env = cjsh_environ(); *env != nullptr; ++env) {
        std::string entry(*env);
        names.insert(entry.substr(0, entry.find('=')));
    }
    std::vector<std::string> result(names.begin(), names.end());
    std::sort(result.begin(), result.end());
    return result;
}

std::string quote_shell_value(const std::string& value) {
    std::string result = "'";
    for (const char c : value) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    return result + "'";
}

void mirror_set_to_process_env(const std::string& name, const std::string& value) {
    if (config::is_posix_mode()) {
        if (getenv(name.c_str()) != nullptr || exported_names.count(name) != 0 ||
            (g_shell && g_shell->get_shell_option(ShellOption::Allexport))) {
            exported_names.insert(name);
            (void)setenv(name.c_str(), value.c_str(), 1);
        }
        return;
    }
    if (!is_process_mirrored_shell_var(name)) {
        return;
    }
    (void)setenv(name.c_str(), value.c_str(), 1);
}

void mirror_unset_from_process_env(const std::string& name) {
    if (config::is_posix_mode()) {
        exported_names.erase(name);
        (void)unsetenv(name.c_str());
        return;
    }
    if (!is_process_mirrored_shell_var(name)) {
        return;
    }
    (void)unsetenv(name.c_str());
}

bool set_shell_or_local_variable_value(Shell* shell, const std::string& name,
                                       const std::string& value) {
    if (shell != nullptr) {
        if (auto* interpreter = shell->get_shell_script_interpreter();
            interpreter && interpreter->is_local_variable(name)) {
            interpreter->set_local_variable(name, value);
            return true;
        }
    }

    return set_shell_variable_value(name, value);
}

bool unset_shell_or_local_variable_value(Shell* shell, const std::string& name) {
    if (shell != nullptr) {
        if (auto* interpreter = shell->get_shell_script_interpreter();
            interpreter && interpreter->is_local_variable(name)) {
            (void)interpreter->unset_local_variable(name);
            return true;
        }
    }

    return unset_shell_variable_value(name);
}

void setup_path_variables(const std::string& paths_file, const std::string& paths_directory) {
    // Read system paths before native startup files, which may override PATH.
    // Clean invocations preserve PATH exactly, including empty and absent values.
    if (config::no_system_paths || config::no_config || config::secure_mode ||
        config::is_posix_mode() || config::no_exec) {
        return;
    }

    // Preserve toolchain precedence and all PATH components in non-login shells.
    // Raw getenv here: PATH bootstrap before shell vars exist.
    const char* inherited_path = getenv("PATH");
    if (!config::login_mode && inherited_path != nullptr && inherited_path[0] != '\0') {
        return;
    }

    std::vector<std::string> paths;
    auto append_paths = [&](const std::string& value) {
        size_t start = 0;
        while (start < value.size()) {
            const size_t end = value.find(':', start);
            const std::string entry = value.substr(start, end - start);
            if (!entry.empty() && std::find(paths.begin(), paths.end(), entry) == paths.end()) {
                paths.push_back(entry);
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    };
    auto read_paths_file = [&](const std::filesystem::path& file) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return;
        }
        std::ifstream stream(file);
        std::string line;
        while (std::getline(stream, line)) {
            line = string_utils::trim_ascii_whitespace_copy(line);
            if (!line.empty() && line.front() != '#') {
                append_paths(line);
            }
        }
    };

    read_paths_file(paths_file);
    std::vector<std::filesystem::path> path_files;
    std::error_code ec;
    std::filesystem::directory_iterator it(
        paths_directory, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->path().filename().string().front() != '.') {
            path_files.push_back(it->path());
        }
    }
    std::sort(path_files.begin(), path_files.end());
    for (const auto& file : path_files) {
        read_paths_file(file);
    }

    // File entries are literal data; no shell expansion or external path_helper
    // process is needed.
    if (inherited_path != nullptr) {
        append_paths(inherited_path);
    }
    if (paths.empty()) {
        append_paths("/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }
    (void)setenv("PATH", string_utils::join_strings(paths, ":").c_str(), 1);
}

std::vector<std::pair<std::string, std::string>> setup_user_system_vars(
    const struct passwd* pw, const std::string& directory) {
    std::vector<std::pair<std::string, std::string>> env_vars;

    // Preserve caller identity labels, even when explicitly empty. Only fill
    // absent labels from the real user's account database.
    if (getenv("USER") == nullptr && pw != nullptr) {
        (void)env_vars.emplace_back("USER", std::string(pw->pw_name));
    }
    if (getenv("LOGNAME") == nullptr && pw != nullptr) {
        (void)env_vars.emplace_back("LOGNAME", std::string(pw->pw_name));
    }

    std::string home_value;
    // Raw getenv here: HOME bootstrap before shell vars exist.
    if (const char* current_home = getenv("HOME");
        current_home != nullptr && current_home[0] != '\0') {
        home_value = current_home;
    } else if (pw != nullptr) {
        home_value = std::string(pw->pw_dir);
        (void)setenv("HOME", home_value.c_str(), 1);
    }
    (void)env_vars.emplace_back("HOME", home_value);

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        (void)env_vars.emplace_back("HOSTNAME", std::string(hostname));
    }

    std::string current_path =
        directory.empty() ? cjsh_filesystem::safe_current_directory() : directory;
    // Startup already resolved this through Built_ins; standalone callers still validate PWD.
    if (const char* inherited_pwd = getenv("PWD");
        directory.empty() && inherited_pwd && inherited_pwd[0] == '/') {
        struct stat logical{};
        struct stat actual{};
        if (stat(inherited_pwd, &logical) == 0 && stat(".", &actual) == 0 &&
            logical.st_dev == actual.st_dev && logical.st_ino == actual.st_ino) {
            current_path = inherited_pwd;
        }
    }

    (void)setenv("PWD", current_path.c_str(), 1);
    (void)env_vars.emplace_back("IFS", std::string(" \t\n"));

    int shlvl = 1;
    // Raw getenv here: SHLVL bootstrap before shell vars exist.
    if (const char* current_shlvl = getenv("SHLVL")) {
        try {
            shlvl = std::stoi(current_shlvl) + 1;
        } catch (...) {
            shlvl = 1;
        }
    }
    std::string shlvl_str = std::to_string(shlvl);
    (void)setenv("SHLVL", shlvl_str.c_str(), 1);

    pipeline_status_utils::set_last_status_env(0);

    auto version_str = get_version();
    (void)env_vars.emplace_back("CJSH_VERSION", version_str);

    // Raw getenv here: PS1 bootstrap before shell vars exist.
    // Older shells exported their built-in prompt, so refresh that inherited default.
    // Startup files are sourced later and can still explicitly select the old template.
    const char* inherited_ps1 = getenv("PS1");
    constexpr const char* legacy_default_ps1 = "\\S  [color=#5fd7ff]\\W[/color] \\g";
    if (inherited_ps1 == nullptr || std::strcmp(inherited_ps1, legacy_default_ps1) == 0) {
        std::string default_ps1 = prompt::default_primary_prompt_template();
        (void)setenv("PS1", default_ps1.c_str(), 1);
    }

    // Raw getenv here: PS2 bootstrap before shell vars exist.
    if (getenv("PS2") == nullptr) {
        std::string default_ps2 = prompt::default_secondary_prompt_template();
        (void)setenv("PS2", default_ps2.c_str(), 1);
    }

    // Raw getenv here: PS4 bootstrap before shell vars exist.
    if (getenv("PS4") == nullptr) {
        (void)setenv("PS4", "+ ", 1);
    }

    return env_vars;
}

bool is_valid_env_name(const std::string& name) {
    return is_valid_identifier(name);
}

std::string get_ifs_delimiters() {
    if (shell_variable_is_set("IFS")) {
        return get_shell_variable_value("IFS");
    }
    return " \t\n";
}

size_t collect_env_assignments(const std::vector<std::string>& args,
                               std::vector<std::pair<std::string, std::string>>& env_assignments) {
    size_t cmd_start_idx = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& token = args[i];
        std::string name;
        std::string value;
        if (parse_env_assignment(token, name, value)) {
            env_assignments.push_back({name, value});
            cmd_start_idx = i + 1;
            continue;
        }
        break;
    }
    return cmd_start_idx;
}

PreparedCommand prepare_command(std::vector<std::string> args) {
    PreparedCommand command;
    command.original_args = std::move(args);
    const size_t start = collect_env_assignments(command.original_args, command.assignments);
    command.args.assign(command.original_args.begin() + static_cast<std::ptrdiff_t>(start),
                        command.original_args.end());
    return command;
}

void apply_env_assignments(
    const std::vector<std::pair<std::string, std::string>>& env_assignments) {
    for (const auto& env : env_assignments) {
        (void)setenv(env.first.c_str(), env.second.c_str(), 1);
    }
}

std::vector<std::string> parse_shell_command(const std::string& command) {
    return command_line_utils::tokenize_shell_words(command);
}

std::vector<char*> build_exec_argv(const std::vector<std::string>& args) {
    static thread_local std::vector<std::unique_ptr<char[]>> arg_buffers;
    arg_buffers.clear();

    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto& arg : args) {
        auto buf = std::make_unique<char[]>(arg.size() + 1);
        (void)std::memcpy(buf.get(), arg.c_str(), arg.size() + 1);
        c_args.push_back(buf.get());
        arg_buffers.push_back(std::move(buf));
    }
    c_args.push_back(nullptr);
    return c_args;
}

bool update_terminal_dimensions() {
    struct winsize ws{};

    const int fds[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    bool updated = false;

    for (int fd : fds) {
        if (fd < 0) {
            continue;
        }

        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && (ws.ws_col > 0 || ws.ws_row > 0)) {
            updated = true;
            break;
        }
    }

    if (!updated) {
        return false;
    }

    if (ws.ws_col > 0) {
        std::string columns = std::to_string(ws.ws_col);
        (void)setenv("COLUMNS", columns.c_str(), 1);
    }

    if (ws.ws_row > 0) {
        std::string lines = std::to_string(ws.ws_row);
        (void)setenv("LINES", lines.c_str(), 1);
    }

    return updated;
}

void sync_env_vars_from_system(Shell& shell) {
    auto& env_map = env_vars();
    for (char** env = cjsh_environ(); *env != nullptr; env++) {
        std::string_view env_str(*env);
        size_t eq_pos = env_str.find('=');
        if (eq_pos != std::string::npos) {
            env_map.insert_or_assign(std::string(env_str.substr(0, eq_pos)),
                                     std::string(env_str.substr(eq_pos + 1)));
        }
    }

    apply_env_vars_to_parser(&shell);
}

std::unordered_map<std::string, std::string>& env_vars() {
    return g_env_vars;
}

void sync_parser_env_vars(Shell* shell) {
    apply_env_vars_to_parser(shell);
}

void sync_parser_env_var(Shell* shell, const std::string& name) {
    if (auto* parser = shell ? shell->get_parser() : nullptr) {
        const auto it = g_env_vars.find(name);
        if (it == g_env_vars.end()) {
            parser->unset_env_var(name);
        } else {
            parser->set_env_var(name, it->second);
        }
    }
}

TemporaryEnvAssignmentScope::TemporaryEnvAssignmentScope(
    Shell* shell, const std::vector<std::pair<std::string, std::string>>& assignments, bool persist)
    : shell_(shell) {
    if (!shell_) {
        return;
    }
    if (!persist) {
        std::unordered_set<std::string> saved;
        backups_.reserve(assignments.size());
        for (const auto& [name, value] : assignments) {
            if (!saved.insert(name).second) {
                continue;
            }
            Backup backup{name, std::nullopt, std::nullopt};
            if (const char* process_value = getenv(name.c_str())) {
                backup.process_value = process_value;
            }
            if (auto it = g_env_vars.find(name); it != g_env_vars.end()) {
                backup.shell_value = it->second;
            }
            backups_.push_back(std::move(backup));
        }
    }
    for (const auto& [name, value] : assignments) {
        g_env_vars[name] = value;
        (void)setenv(name.c_str(), value.c_str(), 1);
        sync_parser_env_var(shell_, name);
    }
}

TemporaryEnvAssignmentScope::~TemporaryEnvAssignmentScope() {
    for (auto it = backups_.rbegin(); it != backups_.rend(); ++it) {
        if (it->process_value) {
            (void)setenv(it->name.c_str(), it->process_value->c_str(), 1);
        } else {
            (void)unsetenv(it->name.c_str());
        }
        if (it->shell_value) {
            g_env_vars[it->name] = *it->shell_value;
        } else {
            (void)g_env_vars.erase(it->name);
        }
        sync_parser_env_var(shell_, it->name);
    }
}

bool exit_requested() {
    return g_exit_flag;
}

void request_exit() {
    g_exit_flag = true;
}

void clear_exit_request() {
    g_exit_flag = false;
}

int posix_error_exit(int status) {
    if (status != 0 && config::is_posix_mode() && !config::interactive_mode) {
        (void)set_shell_variable_value("EXIT_CODE", std::to_string(status));
        request_exit();
    }
    return status;
}

ReplacementShellLevel::ReplacementShellLevel() {
    // Only the exec environment changes; shell variables retain their current level.
    if (const char* value = getenv("SHLVL")) {
        previous = value;
        was_set = true;
        int level = 0;
        if (numeric_utils::parse_int_strict(previous, level) && level > 0) {
            (void)setenv("SHLVL", std::to_string(level - 1).c_str(), 1);
        }
    }
}

ReplacementShellLevel::~ReplacementShellLevel() {
    if (was_set) {
        (void)setenv("SHLVL", previous.c_str(), 1);
    } else {
        (void)unsetenv("SHLVL");
    }
}

bool startup_active() {
    return g_startup_active;
}

void set_startup_active(bool value) {
    g_startup_active = value;
}

std::uint64_t command_sequence() {
    return g_command_sequence;
}

void increment_command_sequence() {
    ++g_command_sequence;
}

void reset_shell_state() {
    exported_names.clear();
    g_exit_flag = false;
    g_startup_active = true;
    g_command_sequence = 0;
}

}  // namespace cjsh_env

int handle_non_interactive_mode(const std::string& script_file) {
    std::string script_content;

    struct ScriptZeroGuard {
        ScriptZeroGuard(Shell* shell, const std::string& new_value) : shell_(shell) {
            // Raw getenv here: preserve process env $0 during script guard.
            const char* current = std::getenv("0");
            if (current != nullptr) {
                previous_ = current;
                had_previous_ = true;
            }
            (void)setenv("0", new_value.c_str(), 1);
            sync_env(new_value.c_str());
            active_ = true;
        }

        ~ScriptZeroGuard() {
            if (!active_) {
                return;
            }
            if (had_previous_) {
                (void)setenv("0", previous_.c_str(), 1);
                sync_env(previous_.c_str());
            } else {
                (void)unsetenv("0");
                sync_env(nullptr);
            }
        }

       private:
        void sync_env(const char* value) {
            if (shell_ == nullptr) {
                return;
            }
            auto& env_map = cjsh_env::env_vars();
            if (value != nullptr) {
                env_map["0"] = value;
            } else {
                (void)env_map.erase("0");
            }
            cjsh_env::sync_parser_env_var(shell_, "0");
        }

        Shell* shell_;
        std::string previous_;
        bool had_previous_{false};
        bool active_{false};
    };

    std::optional<ScriptZeroGuard> zero_guard;
    if (!script_file.empty()) {
        (void)zero_guard.emplace(g_shell.get(), script_file);
    }

    if (!script_file.empty()) {
        auto read_result = cjsh_filesystem::read_file_content(script_file);
        if (!read_result.is_ok()) {
            ErrorType error_type = ErrorType::FILE_NOT_FOUND;
            int exit_code = 127;

            std::error_code status_ec;
            auto status = std::filesystem::status(script_file, status_ec);
            const bool is_directory = !status_ec && std::filesystem::is_directory(status);
            const bool permission_denied =
                read_result.error().find("Permission denied") != std::string::npos;

            if (permission_denied) {
                error_type = ErrorType::PERMISSION_DENIED;
                exit_code = 126;
            } else if (is_directory ||
                       read_result.error().find("Is a directory") != std::string::npos) {
                error_type = ErrorType::RUNTIME_ERROR;
                exit_code = 126;
            }

            print_error({error_type,
                         script_file,
                         read_result.error(),
                         {"Check file path and permissions"}});
            return exit_code;
        }

        script_content = read_result.value();
    } else if ((config::is_posix_mode() || config::read_stdin) && g_shell) {
        // Do not buffer past a complete command: read and external commands
        // must be able to consume subsequent bytes from this same descriptor.
        int status = 0;
        char byte;
        for (;;) {
            (void)g_shell->process_pending_signals();
            if (cjsh_env::exit_requested()) {
                return read_exit_code_or(status);
            }
            const ssize_t count = read(STDIN_FILENO, &byte, 1);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                print_error_errno({ErrorType::RUNTIME_ERROR, "read", "standard input", {}});
                return 1;
            }
            if (count == 0) {
                break;
            }
            script_content += byte;
            if (byte != '\n' || has_line_continuation_suffix(script_content, true)) {
                continue;
            }
            // Validation can parse function bodies and replace the parser's cache.
            const auto lines = g_shell->get_parser()->prepare_interactive_input(script_content);
            if (g_shell->get_parser()->awaiting_here_document()) {
                continue;
            }
            if (g_shell->get_shell_script_interpreter()->needs_additional_input(lines)) {
                continue;
            }
            if (std::all_of(lines.begin(), lines.end(), [](const std::string& line) {
                    return shell_script_interpreter::detail::trim(
                               shell_script_interpreter::detail::strip_inline_comment(line))
                        .empty();
                })) {
                script_content.clear();
                continue;
            }
            status = g_shell->execute(script_content);
            script_content.clear();
        }
        if (!script_content.empty()) {
            status = g_shell->execute(script_content);
        }
        return read_exit_code_or(status);
    } else {
        char buffer[4096];
        for (;;) {
            if (g_shell) {
                (void)g_shell->process_pending_signals();
            }
            if (cjsh_env::exit_requested()) {
                return read_exit_code_or(128 + SignalHandler::termination_signal());
            }
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                script_content.append(buffer, static_cast<size_t>(count));
            } else if (count == 0) {
                break;
            } else if (errno != EINTR) {
                print_error_errno({ErrorType::RUNTIME_ERROR, "read", "standard input", {}});
                return 1;
            }
        }
    }

    if (!script_content.empty()) {
        int code = g_shell ? g_shell->execute(script_content) : 1;
        return read_exit_code_or(code);
    }

    return 0;
}
