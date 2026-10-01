/*
  builtin.cpp

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

#include "builtin.h"

#include "builtin_help.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "alias_abbr_commands.h"
#include "approot_command.h"
#include "boolean_commands.h"
#include "cd_command.h"
#include "cjsh_filesystem.h"
#include "cjshopt_command.h"
#include "command_command.h"
#include "coproc_command.h"
#include "declare_command.h"
#include "directory_stack_commands.h"
#include "echo_command.h"
#include "error_out.h"
#include "eval_command.h"
#include "exec_command.h"
#include "exit_command.h"
#include "export_command.h"
#include "fc_command.h"
#include "firstboot_command.h"
#include "generate_completions_command.h"
#include "getopts_command.h"
#include "hash_command.h"
#include "help_command.h"
#include "history_command.h"
#include "hook_command.h"
#include "job_control_commands.h"
#include "local_command.h"
#include "loop_control_commands.h"
#include "printf_command.h"
#include "pwd_command.h"
#include "read_command.h"
#include "readonly_command.h"
#include "restart_command.h"
#include "runtime_commands.h"
#include "set_command.h"
#include "shell.h"
#include "shell_env.h"
#include "shopt_command.h"
#include "source_command.h"
#include "suggestion_utils.h"
#include "test_command.h"
#include "times_command.h"
#include "trap_command.h"
#include "type_which_command.h"
#include "ulimit_command.h"
#include "umask_command.h"
#include "version_command.h"
#include "widget_command.h"

namespace {

thread_local std::string regular_builtin;
thread_local bool executing_special_builtin = false;

struct SpecialBuiltinScope {
    bool previous;
    explicit SpecialBuiltinScope(bool value) : previous(executing_special_builtin) {
        executing_special_builtin = value;
    }
    ~SpecialBuiltinScope() {
        executing_special_builtin = previous;
    }
};

bool is_posix_restricted_builtin(const std::string& name) {
    return name == "abbr" || name == "abbreviate" || name == "approot" || name == "pushd" ||
           name == "popd" || name == "dirs" || name == "unabbr" || name == "unabbreviate" ||
           name == "help" || name == "version" || name == "history" || name == "restart" ||
           name == "which" || name == "jobname" || name == "disown" ||
           name == "generate-completions" || name == "firstboot" || name == "hook" ||
           name == "cjsh-widget" || name == "shopt" || name == "builtin" || name == "quit" ||
           name == "bye" || name == "suspend" || name == "[[";
}

int reject_posix_restricted_builtin(const std::string& name) {
    print_error({ErrorType::INVALID_ARGUMENT,
                 name,
                 "'" + name + "' is not available in POSIX mode",
                 {"Run without --posix to use cjsh-specific builtins"}});
    return 1;
}

}  // namespace

RegularBuiltinScope::RegularBuiltinScope(std::string name) : previous(std::move(regular_builtin)) {
    regular_builtin = std::move(name);
}

RegularBuiltinScope::~RegularBuiltinScope() {
    regular_builtin = std::move(previous);
}

int posix_special_builtin_error(int status) {
    return executing_special_builtin ? cjsh_env::posix_error_exit(status) : status;
}

bool is_posix_special_builtin(const std::string& name) {
    return name == "." || name == ":" || name == "break" || name == "continue" || name == "eval" ||
           name == "exec" || name == "exit" || name == "export" || name == "readonly" ||
           name == "return" || name == "set" || name == "shift" || name == "times" ||
           name == "trap" || name == "unset";
}

Built_ins::Built_ins() : shell(nullptr) {
    builtins = {
        {"shopt", [this](const std::vector<std::string>& args) { return ::shopt_command(args, shell); }},
        {"echo", [](const std::vector<std::string>& args) { return ::echo_command(args); }},
        {"printf", [](const std::vector<std::string>& args) { return ::printf_command(args); }},
        {"pwd", [](const std::vector<std::string>& args) { return ::pwd_command(args); }},
        {"true",
         [](const std::vector<std::string>& args) {
             if (builtin_handle_help(args,
                                     {"Usage: true", "Return a successful status (exit code 0).",
                                      "Any additional arguments are ignored."})) {
                 return 0;
             }
             return ::true_command();
         }},
        {"false",
         [](const std::vector<std::string>& args) {
             if (builtin_handle_help(args,
                                     {"Usage: false", "Return a failing status (exit code 1).",
                                      "Any additional arguments are ignored."})) {
                 return 0;
             }
             return ::false_command();
         }},
        {"cd",
         [this](const std::vector<std::string>& args) {
             return ::cd_command(args, current_directory, previous_directory, shell);
         }},
        {"approot",
         [this](const std::vector<std::string>& args) {
             return ::approot_command(args, current_directory, previous_directory, shell);
         }},
        {"pushd",
         [this](const std::vector<std::string>& args) {
             return ::pushd_command(args, current_directory, previous_directory, shell);
         }},
        {"popd",
         [this](const std::vector<std::string>& args) {
             return ::popd_command(args, current_directory, previous_directory, shell);
         }},
        {"dirs",
         [this](const std::vector<std::string>& args) {
             return ::dirs_command(args, current_directory, shell);
         }},
        {"local",
         [this](const std::vector<std::string>& args) { return ::local_command(args, shell); }},
        {"declare",
         [this](const std::vector<std::string>& args) { return ::declare_command(args, shell); }},
        {"typeset",
         [this](const std::vector<std::string>& args) { return ::declare_command(args, shell); }},
        {"coproc",
         [this](const std::vector<std::string>& args) { return ::coproc_command(args, shell); }},
        {"alias",
         [this](const std::vector<std::string>& args) { return ::alias_command(args, shell); }},
        {"abbr",
         [this](const std::vector<std::string>& args) { return ::abbr_command(args, shell); }},
        {"abbreviate",
         [this](const std::vector<std::string>& args) { return ::abbr_command(args, shell); }},
        {"export",
         [this](const std::vector<std::string>& args) { return ::export_command(args, shell); }},
        {"unalias",
         [this](const std::vector<std::string>& args) { return ::unalias_command(args, shell); }},
        {"unabbr",
         [this](const std::vector<std::string>& args) { return ::unabbr_command(args, shell); }},
        {"unabbreviate",
         [this](const std::vector<std::string>& args) { return ::unabbr_command(args, shell); }},
        {"unset",
         [this](const std::vector<std::string>& args) { return ::unset_command(args, shell); }},
        {"set",
         [this](const std::vector<std::string>& args) { return ::set_command(args, shell); }},
        {"shift",
         [this](const std::vector<std::string>& args) { return ::shift_command(args, shell); }},
        {"break", [](const std::vector<std::string>& args) { return ::break_command(args); }},
        {"continue", [](const std::vector<std::string>& args) { return ::continue_command(args); }},
        {"return", [](const std::vector<std::string>& args) { return ::return_command(args); }},
        {"source", [](const std::vector<std::string>& args) { return ::source_command(args); }},
        {".", [](const std::vector<std::string>& args) { return ::source_command(args); }},
        {"help",
         [](const std::vector<std::string>& args) {
             if (builtin_handle_help(args,
                                     {"Usage: help", "Display the CJSH command reference."})) {
                 return 0;
             }
             return ::help_command();
         }},
        {"hash", [](const std::vector<std::string>& args) { return ::hash_command(args); }},
        {"version", [](const std::vector<std::string>& args) { return ::version_command(args); }},
        {"eval",
         [this](const std::vector<std::string>& args) { return ::eval_command(args, shell); }},
        {"history", [](const std::vector<std::string>& args) { return ::history_command(args); }},
        {"fc", [this](const std::vector<std::string>& args) { return ::fc_command(args, shell); }},
        {"exit", [](const std::vector<std::string>& args) { return ::exit_command(args); }},
        {"quit", [](const std::vector<std::string>& args) { return ::exit_command(args); }},
        {"bye", [](const std::vector<std::string>& args) { return ::exit_command(args); }},
        {"restart", [](const std::vector<std::string>& args) { return ::restart_command(args); }},
        {"firstboot", [](const std::vector<std::string>& args) { return ::firstboot_command(args); }},
        {"test", [](const std::vector<std::string>& args) { return ::test_command(args); }},
        {"[", [](const std::vector<std::string>& args) { return ::test_command(args); }},
        {"exec",
         [this](const std::vector<std::string>& args) { return ::exec_command(args, shell); }},
        {":",
         [](const std::vector<std::string>& args) {
             if (builtin_handle_help(args,
                                     {"Usage: :", "Null command that does nothing and succeeds.",
                                      "Any additional arguments are ignored."})) {
                 return 0;
             }
             return 0;
         }},
        {"trap", [](const std::vector<std::string>& args) { return ::trap_command(args); }},
        {"jobs", [](const std::vector<std::string>& args) { return ::jobs_command(args); }},
        {"jobname", [](const std::vector<std::string>& args) { return ::jobname_command(args); }},
        {"fg", [](const std::vector<std::string>& args) { return ::fg_command(args); }},
        {"bg", [](const std::vector<std::string>& args) { return ::bg_command(args); }},
        {"wait", [](const std::vector<std::string>& args) { return ::wait_command(args); }},
        {"kill", [](const std::vector<std::string>& args) { return ::kill_command(args); }},
        {"disown", [](const std::vector<std::string>& args) { return ::disown_command(args); }},
        {"suspend", [](const std::vector<std::string>& args) { return ::suspend_command(args); }},
        {"readonly", [](const std::vector<std::string>& args) { return ::readonly_command(args); }},
        {"read",
         [this](const std::vector<std::string>& args) { return ::read_command(args, shell); }},
        {"umask", [](const std::vector<std::string>& args) { return ::umask_command(args); }},
        {"ulimit", [](const std::vector<std::string>& args) { return ::ulimit_command(args); }},

        {"getopts",
         [this](const std::vector<std::string>& args) { return ::getopts_command(args, shell); }},
        {"times", [](const std::vector<std::string>& args) { return ::times_command(args); }},
        {"type",
         [this](const std::vector<std::string>& args) { return ::type_command(args, shell); }},
        {"which",
         [this](const std::vector<std::string>& args) { return ::which_command(args, shell); }},
        {"generate-completions",
         [this](const std::vector<std::string>& args) {
             return ::generate_completions_command(args, shell);
         }},
        {"hook",
         [this](const std::vector<std::string>& args) { return ::hook_command(args, shell); }},
        {"command",
         [this](const std::vector<std::string>& args) { return ::command_command(args, shell); }},
        {"cjsh-widget",
         [](const std::vector<std::string>& args) { return ::widget_builtin(args); }},
        {"builtin",
         [this](const std::vector<std::string>& args) {
             if (builtin_handle_help(
                     args, {"Usage: builtin COMMAND [ARGS...]",
                            "Invoke a builtin command bypassing functions and PATH lookup."})) {
                 return 0;
             }
             if (args.size() < 2) {
                 ErrorInfo error = {ErrorType::INVALID_ARGUMENT,
                                    "builtin",
                                    "missing command operand",
                                    {"Usage: builtin <command> [args...]"}};
                 print_error(error);
                 return 2;
             }

             const std::string& target_command = args[1];
             if (target_command == "builtin") {
                 ErrorInfo error = {ErrorType::INVALID_ARGUMENT,
                                    "builtin",
                                    "cannot invoke builtin recursively",
                                    {"Usage: builtin <command> [args...]"}};
                 print_error(error);
                 return 2;
             }

             if (config::is_posix_mode() && is_posix_restricted_builtin(target_command)) {
                 return reject_posix_restricted_builtin(target_command);
             }

             auto builtin_it = builtins.find(target_command);
             if (builtin_it == builtins.end()) {
                 ErrorInfo error = {ErrorType::INVALID_ARGUMENT,
                                    "builtin",
                                    "'" + target_command + "' is not a builtin command",
                                    {"Use 'help' to list available builtins"}};
                 print_error(error);
                 return 1;
             }

             std::vector<std::string> forwarded_args(args.begin() + 1, args.end());
             const int status = builtin_it->second(forwarded_args);
             if (target_command == "firstboot" && !cjsh_filesystem::is_first_boot()) {
                 builtins.erase(target_command);
             }
             return status;
         }},
        {"cjshopt", [](const std::vector<std::string>& args) { return ::cjshopt_command(args); }},
    };

    if (!cjsh_filesystem::is_first_boot()) {
        builtins.erase("firstboot");
    }
}

Built_ins::~Built_ins() = default;

void Built_ins::set_shell(Shell* shell_ptr) {
    shell = shell_ptr;
}

std::string Built_ins::get_current_directory() const {
    return current_directory;
}

std::string Built_ins::get_previous_directory() const {
    return previous_directory;
}

void Built_ins::set_current_directory() {
    std::string physical_directory = cjsh_filesystem::safe_current_directory();
    current_directory = physical_directory;

    if (!cjsh_env::shell_variable_is_set("PWD")) {
        return;
    }

    std::string logical_directory = cjsh_env::get_shell_variable_value("PWD");
    if (logical_directory.empty() || logical_directory[0] != '/') {
        return;
    }

    struct stat logical_stat{};
    struct stat physical_stat{};
    if (stat(logical_directory.c_str(), &logical_stat) != 0 ||
        stat(physical_directory.c_str(), &physical_stat) != 0) {
        return;
    }

    if (logical_stat.st_dev == physical_stat.st_dev &&
        logical_stat.st_ino == physical_stat.st_ino) {
        current_directory = logical_directory;
    }
}

std::vector<std::string> Built_ins::get_builtin_commands() const {
    std::vector<std::string> names;
    names.reserve(builtins.size());
    for (const auto& kv : builtins) {
        names.push_back(kv.first);
    }
    return names;
}

int Built_ins::builtin_command(const std::vector<std::string>& args) {
    if (args.empty()) {
        return 1;
    }

    auto it = builtins.find(args[0]);
    if (it != builtins.end()) {
        if (config::is_posix_mode() && args[0] == "cjshopt" &&
            (args.size() < 2 || args[1] != "dialect")) {
            return reject_posix_restricted_builtin(args[0]);
        }
        if (config::is_posix_mode() && is_posix_restricted_builtin(args[0])) {
            return reject_posix_restricted_builtin(args[0]);
        }
        const bool special = is_posix_special_builtin(args[0]) && regular_builtin != args[0];
        // Consume the override so eval and dot do not suppress nested errors.
        RegularBuiltinScope nested_scope("");
        SpecialBuiltinScope special_scope(special);
        int status = it->second(args);
        // eval and dot return ordinary command statuses as well as their own
        // errors. Their errors are marked at the point of detection.
        if (special && status > 0 && args[0] != "eval" && args[0] != "." && args[0] != "return" &&
            args[0] != "break" && args[0] != "continue") {
            (void)cjsh_env::posix_error_exit(status);
        }
        if (args[0] == "firstboot" && !cjsh_filesystem::is_first_boot()) {
            builtins.erase(args[0]);
        }
        return status;
    }
    std::vector<std::string> suggestions;
    suggestions = suggestion_utils::generate_command_suggestions_if_enabled(args[0]);

    ErrorInfo error = {ErrorType::COMMAND_NOT_FOUND, args[0], "", suggestions};
    print_error(error);
    return 127;
}

int Built_ins::is_builtin_command(const std::string& cmd) const {
    if (cmd.empty()) {
        return 0;
    }

    return builtins.find(cmd) != builtins.end();
}

int Built_ins::builtin_or_runtime_command(const std::vector<std::string>& args) {
    if (!args.empty() && runtime_commands::is_runtime_command_name(args[0])) {
        return runtime_commands::execute_runtime_command(args, shell);
    }

    return builtin_command(args);
}

int Built_ins::is_builtin_or_runtime_command(const std::string& cmd) const {
    if (is_builtin_command(cmd) != 0) {
        return 1;
    }

    return runtime_commands::is_runtime_command_name(cmd) ? 1 : 0;
}
