/*
  shell.h

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

#ifndef CJSH_CORE_SRC_CORE_SHELL_H
#define CJSH_CORE_SRC_CORE_SHELL_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <termios.h>

#include "error_out.h"
#include "parser.h"
#include "signal_handler.h"

namespace cjsh_env {
struct PreparedCommand;
}

class Exec;
class Built_ins;
class ShellScriptInterpreter;
struct Command;

enum class ShellOption : std::uint8_t {
    Errexit,
    Noclobber,
    Nounset,
    Xtrace,
    Verbose,
    Noexec,
    Noglob,
    Globstar,
    Allexport,
    Huponexit,
    Pipefail,
    Monitor,
    Hashall,
    Notify,
    Ignoreeof,
    Nolog,
    Extglob,
    ExpandAliases,
    BraceExpand,
    HistExpand,
    Autocd,
    Count
};

struct ShellOptionDescriptor {
    ShellOption option;
    char short_flag;
    const char* name;
    bool shopt = false;
};

const std::array<ShellOptionDescriptor, static_cast<size_t>(ShellOption::Count)>&
get_shell_option_descriptors();
std::optional<ShellOption> parse_shell_option(const std::string& name);
std::optional<ShellOption> parse_shopt_option(const std::string& name);
std::optional<ShellOption> parse_shell_option_short(char short_flag);

enum class HookType : std::uint8_t {
    Precmd,
    Preexec,
    Chpwd,
    Idle,
    Count
};

struct HookTypeDescriptor {
    HookType type;
    const char* name;
};

const std::array<HookTypeDescriptor, static_cast<size_t>(HookType::Count)>&
get_hook_type_descriptors();
std::optional<HookType> parse_hook_type(const std::string& name);

class Shell {
   public:
    Shell();
    ~Shell();
    void run_exit_handlers(int status);
    int execute(const std::string& script, bool skip_validation = false);
    int execute_command(std::vector<std::string> args, bool run_in_background = false,
                        bool auto_background_on_stop = false,
                        bool auto_background_on_stop_silent = false);
    int execute_prepared_command(cjsh_env::PreparedCommand command, bool run_in_background = false,
                                 bool auto_background_on_stop = false,
                                 bool auto_background_on_stop_silent = false);
    int execute_script_file(const std::filesystem::path& path, bool optional = false);
    int execute_script_content(const std::string& content, const std::string& source_path);

    SignalProcessingResult process_pending_signals(bool reap_children = true);
    void setup_signal_handlers();
    void setup_interactive_handlers();
    void save_terminal_state();
    void restore_terminal_state();
    void setup_job_control();
    bool reclaim_terminal() const;
    void mark_terminal_dirty() {
        prompt_terminal_dirty.store(true, std::memory_order_relaxed);
    }
    void recover_prompt_terminal();
    bool suspend() const;
    bool manages_terminal() const;
    bool is_job_control_enabled() const;
    bool set_job_control_enabled(bool enabled);

    void set_interactive_mode(bool flag);
    void begin_interactive_input() {
        interactive_input_started = true;
    }
    bool get_interactive_mode() const;
    bool is_interactive_process() const;
    void set_abbreviations(const std::unordered_map<std::string, std::string>& new_abbreviations);
    std::unordered_map<std::string, std::string>& get_abbreviations();
    void set_aliases(const std::unordered_map<std::string, std::string>& new_aliases);
    std::unordered_map<std::string, std::string>& get_aliases();

    std::vector<std::string>& get_directory_stack();
    const std::vector<std::string>& get_directory_stack() const;
    void set_last_interactive_command(const std::string& command);
    const std::string& get_last_interactive_command() const;

    void register_hook(HookType hook_type, const std::string& function_name);
    void unregister_hook(HookType hook_type, const std::string& function_name);
    std::vector<std::string> get_hooks(HookType hook_type) const;
    void clear_hooks(HookType hook_type);
    void execute_hooks(HookType hook_type, const std::vector<std::string>& arguments = {});

    void apply_startup_options(const std::vector<std::pair<std::string, bool>>& options);
    void set_shell_option(ShellOption option, bool value);
    bool get_shell_option(ShellOption option) const;
    bool is_errexit_enabled() const;
    void set_errexit_severity(const std::string& severity);
    std::string get_errexit_severity() const;
    bool should_abort_on_nonzero_exit() const;
    bool should_abort_on_nonzero_exit(int exit_code) const;
    class ErrexitScope {
       public:
        explicit ErrexitScope(Shell* shell, bool suppress = true)
            : shell_(suppress ? shell : nullptr) {
            if (shell_) {
                ++shell_->errexit_suppression_depth;
            }
        }
        ~ErrexitScope() {
            if (shell_) {
                --shell_->errexit_suppression_depth;
            }
        }
        ErrexitScope(const ErrexitScope&) = delete;
        ErrexitScope& operator=(const ErrexitScope&) = delete;

       private:
        Shell* shell_;
    };

    std::unordered_set<std::string> get_available_commands() const;
    std::string get_previous_directory() const;
    Built_ins* get_built_ins();
    ShellScriptInterpreter* get_shell_script_interpreter();
    Parser* get_parser();

    std::string last_command;
    std::unique_ptr<Exec> shell_exec;

   private:
    bool interactive_mode = false;
    pid_t shell_pid;
    bool interactive_input_started = false;
    bool exit_handlers_invoked = false;
    int shell_terminal = -1;
    bool owns_shell_terminal = false;
    pid_t shell_pgid = 0;
    struct termios shell_tmodes;
    bool terminal_state_saved = false;
    std::atomic<bool> prompt_terminal_dirty{true};
    bool job_control_enabled = false;
    bool interactive_job_control_available = false;

    std::unique_ptr<SignalHandler> signal_handler;
    std::unique_ptr<Built_ins> built_ins;
    std::unique_ptr<Parser> shell_parser;
    std::unique_ptr<ShellScriptInterpreter> shell_script_interpreter;

    std::unordered_map<std::string, std::string> abbreviations;
    std::unordered_map<std::string, std::string> aliases;
    std::array<bool, static_cast<size_t>(ShellOption::Count)> shell_options{};
    std::array<bool, static_cast<size_t>(ShellOption::Count)> explicit_shell_options{};
    std::vector<std::string> directory_stack;
    std::string last_interactive_command;
    ErrorSeverity errexit_severity_level = ErrorSeverity::ERROR;
    unsigned errexit_suppression_depth = 0;

    std::array<std::vector<std::string>, static_cast<size_t>(HookType::Count)> hooks;
    std::string last_directory;

    void apply_abbreviations_to_line_editor();
};

extern std::unique_ptr<Shell> g_shell;

int read_exit_code_or(int fallback);

#endif  // CJSH_CORE_SRC_CORE_SHELL_H
