/*
  shell_env.h

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

#ifndef CJSH_CORE_SRC_CORE_SHELL_ENV_H
#define CJSH_CORE_SRC_CORE_SHELL_ENV_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct passwd;
class Shell;

namespace config {
enum class ExitConfirmationMode : std::uint8_t {
    Smart,
    Always,
    Never
};

extern bool login_mode;
extern bool interactive_mode;
extern bool force_interactive;
extern bool execute_command;
extern bool read_stdin;
extern std::string cmd_to_execute;
extern bool no_exec;
extern bool no_config;
extern bool no_system_paths;
extern std::string config_directory;
extern bool cache_persistence_enabled;
extern bool history_persistence_enabled;
extern bool colors_enabled;
extern bool source_enabled;
extern bool completions_enabled;
extern bool completion_learning_enabled;
extern bool smart_cd_enabled;
extern bool extglob_enabled;
extern bool syntax_highlighting_enabled;
extern bool show_version;
extern bool show_help;
extern bool minimal_mode;
extern bool show_startup_time;
extern bool secure_mode;
extern bool show_title_line;
extern bool history_enabled;
extern bool history_expansion_enabled;
extern bool newline_after_execution;
extern bool suppress_sh_warning;
extern bool status_line_enabled;
extern bool status_reporting_enabled;
extern bool script_extension_interpreter_enabled;
extern bool error_suggestions_enabled;
extern bool prompt_vars_enabled;
extern long idle_timeout_seconds;
extern ExitConfirmationMode exit_confirmation_mode;
}  // namespace config

namespace cjsh_env {

void setup_environment_variables(const char* argv0 = nullptr);
void setup_path_variables(const std::string& paths_file = "/etc/paths",
                          const std::string& paths_directory = "/etc/paths.d");
std::vector<std::pair<std::string, std::string>> setup_user_system_vars(
    const struct passwd* pw, const std::string& directory = "");

std::string get_shell_variable_value(const std::string& name);
std::string get_shell_variable_value(const char* name);
bool shell_variable_is_set(const std::string& name);
bool shell_variable_is_set(const char* name);
bool set_shell_variable_value(const std::string& name, const std::string& value);
bool unset_shell_variable_value(const std::string& name);
bool set_shell_or_local_variable_value(Shell* shell, const std::string& name,
                                       const std::string& value);
bool unset_shell_or_local_variable_value(Shell* shell, const std::string& name);

bool update_terminal_dimensions();
void sync_env_vars_from_system(Shell& shell);
std::unordered_map<std::string, std::string>& env_vars();
void sync_parser_env_var(Shell* shell, const std::string& name);
bool should_mirror_to_process_env(const std::string& name);
void mirror_set_to_process_env(const std::string& name, const std::string& value);
void mirror_unset_from_process_env(const std::string& name);
void mark_exported(const std::string& name);
std::vector<std::string> exported_variable_names();
std::string quote_shell_value(const std::string& value);

bool exit_requested();
void request_exit();
void clear_exit_request();
// POSIX shell errors terminate a noninteractive execution environment, independent of -e.
int posix_error_exit(int status);

bool startup_active();
void set_startup_active(bool value);

std::uint64_t command_sequence();
void increment_command_sequence();

void reset_shell_state();

struct PreparedCommand {
    std::vector<std::string> original_args;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> assignments;
    std::optional<bool> is_builtin;
};

PreparedCommand prepare_command(std::vector<std::string> args);

class TemporaryEnvAssignmentScope {
   public:
    TemporaryEnvAssignmentScope(Shell* shell,
                                const std::vector<std::pair<std::string, std::string>>& assignments,
                                bool persist = false);
    ~TemporaryEnvAssignmentScope();
    TemporaryEnvAssignmentScope(const TemporaryEnvAssignmentScope&) = delete;
    TemporaryEnvAssignmentScope& operator=(const TemporaryEnvAssignmentScope&) = delete;

   private:
    struct Backup {
        std::string name;
        std::optional<std::string> process_value;
        std::optional<std::string> shell_value;
    };
    Shell* shell_;
    std::vector<Backup> backups_;
};

bool is_valid_env_name(const std::string& name);
std::string get_ifs_delimiters();
size_t collect_env_assignments(const std::vector<std::string>& args,
                               std::vector<std::pair<std::string, std::string>>& env_assignments);
void apply_env_assignments(const std::vector<std::pair<std::string, std::string>>& env_assignments);
std::vector<std::string> parse_shell_command(const std::string& command);
std::vector<char*> build_exec_argv(const std::vector<std::string>& args);

class ReplacementShellLevel {
   public:
    ReplacementShellLevel();
    ~ReplacementShellLevel();
    ReplacementShellLevel(const ReplacementShellLevel&) = delete;
    ReplacementShellLevel& operator=(const ReplacementShellLevel&) = delete;

   private:
    std::string previous;
    bool was_set = false;
};

}  // namespace cjsh_env

int handle_non_interactive_mode(const std::string& script_file);

#endif  // CJSH_CORE_SRC_CORE_SHELL_ENV_H
