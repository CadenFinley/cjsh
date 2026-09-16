/*
  builtins_completions_handler.cpp

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

#include "builtins_completions_handler.h"
#include <sys/types.h>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cjsh_filesystem.h"
#include "completion_utils.h"
#include "job_control.h"
#include "signal_handler.h"

namespace builtin_completions {
namespace {

CommandDoc make_doc(std::string summary, std::vector<CompletionEntry> entries) {
    CommandDoc doc;
    doc.summary = std::move(summary);
    doc.summary_present = !doc.summary.empty();
    doc.entries = std::move(entries);
    doc.executable_path.clear();
    return doc;
}

CompletionEntry make_option(std::string text, std::string description) {
    return CompletionEntry{std::move(text), std::move(description), EntryKind::Option};
}

CompletionEntry make_value_option(std::string text, std::vector<std::string> aliases,
                                  std::string description, ValueRequirement requirement,
                                  ValueType type, std::string value_name,
                                  ValueSeparator separator = ValueSeparator::Space) {
    CompletionEntry entry{std::move(text), std::move(description), EntryKind::Option};
    entry.aliases = std::move(aliases);
    entry.value.requirement = requirement;
    entry.value.type = type;
    entry.value.name = std::move(value_name);
    entry.value.separator = separator;
    return entry;
}

CompletionEntry make_positional(std::string name, std::string description, ValueType type,
                                std::size_t index, bool variadic = false) {
    CompletionEntry entry{std::move(name), std::move(description), EntryKind::Positional};
    entry.value.requirement = ValueRequirement::Required;
    entry.value.type = type;
    entry.value.name = entry.text;
    entry.positional_index = index;
    entry.variadic = variadic;
    return entry;
}

CompletionEntry make_subcommand(std::string text, std::string description) {
    return CompletionEntry{std::move(text), std::move(description), EntryKind::Subcommand};
}

constexpr const char kKillSummary[] = "Send signals to processes or jobs";
constexpr const char kTrapSummary[] = "Set or list signal handlers";

std::string strip_sig_prefix(const std::string& value) {
    if (value.size() > 3 && value.rfind("SIG", 0) == 0) {
        return value.substr(3);
    }
    return value;
}

void append_unique_signal_entry(std::vector<CompletionEntry>& entries,
                                std::unordered_set<std::string>& seen_tokens,
                                const std::string& token, const char* description) {
    if (!token.empty() && seen_tokens.insert(token).second) {
        entries.push_back(make_option(token, description ? description : ""));
    }
}

template <typename Formatter>
void append_available_signal_entries(std::vector<CompletionEntry>& entries,
                                     std::unordered_set<std::string>& seen_tokens,
                                     Formatter&& format_token) {
    for (const auto& info : SignalHandler::available_signals()) {
        if (info.name == nullptr) {
            continue;
        }
        std::string full_name(info.name);
        append_unique_signal_entry(entries, seen_tokens, format_token(strip_sig_prefix(full_name)),
                                   info.description);
        append_unique_signal_entry(entries, seen_tokens, format_token(full_name), info.description);
        append_unique_signal_entry(entries, seen_tokens, format_token(std::to_string(info.signal)),
                                   info.description);
    }
}

void append_kill_signal_entries(std::vector<CompletionEntry>& entries) {
    const auto& signals = SignalHandler::available_signals();
    if (signals.empty()) {
        return;
    }

    std::unordered_set<std::string> seen_tokens;
    seen_tokens.reserve(signals.size() * 3 + 2);

    append_available_signal_entries(entries, seen_tokens,
                                    [](const std::string& token) { return "-" + token; });
    append_unique_signal_entry(entries, seen_tokens, "-0",
                               "Test for process existence without delivering a signal");
}

void append_trap_signal_entries(std::vector<CompletionEntry>& entries) {
    const auto& signals = SignalHandler::available_signals();
    if (signals.empty()) {
        return;
    }

    std::unordered_set<std::string> seen_tokens;
    seen_tokens.reserve(signals.size() * 3 + 4);

    append_available_signal_entries(entries, seen_tokens,
                                    [](const std::string& token) { return token; });
    append_unique_signal_entry(entries, seen_tokens, "EXIT", "Run when the shell exits");
    append_unique_signal_entry(entries, seen_tokens, "0", "Run when the shell exits");
}

std::string format_job_description(const JobControlJob& job) {
    const std::string& source = job.has_custom_name() ? job.custom_name : job.command;
    std::string summary = completion_utils::sanitize_job_command_summary(source);
    if (summary.empty()) {
        summary = "command unavailable";
    }
    return "job %" + std::to_string(job.job_id) + " · " + summary;
}

void append_kill_job_pid_entries(std::vector<CompletionEntry>& entries) {
    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();
    auto jobs = job_manager.get_all_jobs();
    if (jobs.empty()) {
        return;
    }

    std::unordered_set<long long> seen_pids;
    seen_pids.reserve(jobs.size() * 2);

    auto add_pid_entry = [&](const std::shared_ptr<JobControlJob>& job, pid_t pid) {
        if (!job || pid <= 0) {
            return;
        }
        long long pid_value = static_cast<long long>(pid);
        if (!seen_pids.insert(pid_value).second) {
            return;
        }
        entries.push_back(make_option(std::to_string(pid_value), format_job_description(*job)));
    };

    for (const auto& job : jobs) {
        if (!job) {
            continue;
        }
        for (pid_t pid : job->pids) {
            add_pid_entry(job, pid);
        }
        if (job->pgid > 0) {
            add_pid_entry(job, job->pgid);
        }
    }
}

CommandDoc make_kill_command_doc() {
    CommandDoc doc;
    doc.summary = kKillSummary;
    doc.summary_present = true;
    doc.entries = {make_option("-l", "List signal names"),
                   make_option("-s", "Specify signal by name"),
                   make_option("-n", "Specify signal by number")};

    append_kill_signal_entries(doc.entries);
    append_kill_job_pid_entries(doc.entries);
    return doc;
}

CommandDoc make_trap_command_doc() {
    CommandDoc doc;
    doc.summary = kTrapSummary;
    doc.summary_present = true;
    doc.entries = {make_option("-l", "List available signals"),
                   make_option("-p", "Show current traps")};

    append_trap_signal_entries(doc.entries);
    return doc;
}

const CommandDoc* lookup_dynamic_builtin_doc(const std::string& doc_target) {
    if (doc_target == "kill") {
        thread_local CommandDoc kill_doc;
        kill_doc = make_kill_command_doc();
        return &kill_doc;
    }
    if (doc_target == "trap") {
        thread_local CommandDoc trap_doc;
        trap_doc = make_trap_command_doc();
        return &trap_doc;
    }
    return nullptr;
}

const std::unordered_map<std::string, CommandDoc>& builtin_command_docs() {
    static const std::unordered_map<std::string, CommandDoc> docs = [] {
        std::unordered_map<std::string, CommandDoc> map;

        auto add_doc = [&](std::string key, std::string summary,
                           std::vector<CompletionEntry> entries) {
            (void)map.emplace(std::move(key), make_doc(std::move(summary), std::move(entries)));
        };

        auto add_alias = [&](const std::string& alias, const std::string& target) {
            auto it = map.find(target);
            if (it != map.end()) {
                (void)map.emplace(alias, it->second);
            }
        };

        add_doc("abbr", "Manage interactive abbreviations", {});
        add_doc("unabbr", "Remove interactive abbreviations", {});
        add_alias("abbreviate", "abbr");
        add_alias("unabbreviate", "unabbr");

        add_doc("alias", "Create or inspect command aliases",
                {make_option("-p", "Print aliases in reusable form")});
        add_doc("unalias", "Remove command aliases", {make_option("-a", "Remove all aliases")});

        add_doc(
            "cjsh", "POSIX Shell Scripting meets Modern Shell Features",
            {make_option("-h", "Display help message and exit"),
             make_option("--help", "Display help message and exit"),
             make_option("-v", "Print version information and exit"),
             make_option("--version", "Print version information and exit"),
             make_option("-l", "Start as a login shell"),
             make_option("--login", "Start as a login shell (load ~/.cjprofile)"),
             make_option("-i", "Force interactive mode"),
             make_option("--interactive", "Force interactive mode"),
             make_value_option("--command", {"-c"}, "Execute the specified command string and exit",
                               ValueRequirement::Required, ValueType::Text, "COMMAND",
                               ValueSeparator::Either),
             make_option("-n", "Check syntax without executing commands"),
             make_option("--no-exec", "Check syntax without executing commands"),
             make_option("--no-config", "Skip automatic startup and logout configuration"),
             make_value_option("--config-dir", {}, "Override the native configuration root",
                               ValueRequirement::Required, ValueType::Directory, "DIR"),
             make_option("--no-system-paths", "Skip PATH setup from /etc/paths and /etc/paths.d"),
             make_option("--posix", "Enable POSIX mode and reject non-POSIX syntax"),
             make_option("-m", "Disable cjsh enhancements"),
             make_option("--minimal", "Disable cjsh enhancements"),
             make_option("-C", "Disable color output"),
             make_option("--no-colors", "Disable color output"),
             make_option("-N", "Skip sourcing ~/.cjshrc"),
             make_option("--no-source", "Skip sourcing ~/.cjshrc"),
             make_option("-O", "Disable tab completions"),
             make_option("--no-completions", "Disable tab completions"),
             make_option("--no-completion-learning", "Disable on-demand completion learning"),
             make_option("--no-script-extension-interpreter",
                         "Disable extension-based script runners"),
             make_option("--no-smart-cd", "Disable smart cd auto-jumps"),
             make_option("-S", "Disable syntax highlighting"),
             make_option("--no-syntax-highlighting", "Disable syntax highlighting"),
             make_option("--no-error-suggestions", "Disable error suggestions"),
             make_option("--no-agent", "Disable agent-assisted command writing"),
             make_option("--no-prompt-vars", "Ignore PS1/PS2 prompt variables"),
             make_option("--no-history", "Disable history recording and history expansion"),
             make_option("-H", "Disable history expansion"),
             make_option("--no-history-expansion", "Disable history expansion (!commands)"),
             make_option("-W", "Suppress the sh invocation warning"),
             make_option("--no-sh-warning", "Suppress the sh invocation warning"),
             make_option("-L", "Disable title line on startup"),
             make_option("--no-titleline", "Disable title line on startup"),
             make_option("-U", "Display startup time"),
             make_option("--show-startup-time", "Display startup time"),
             make_option("-s", "Secure mode: disable cjshenv/profile/rc/logout files"),
             make_option("--secure", "Secure mode: disable cjshenv/profile/rc/logout files")});

        add_doc("break", "Exit the innermost enclosing loop", {});
        add_doc("continue", "Advance to the next loop iteration", {});
        add_doc("return", "Exit the current function with an optional status", {});

        add_doc("cd", "Change the current directory", {});
        add_doc("approot", "Change directory to cjsh app roots",
                {make_option("-p", "Print resolved directory without changing to it"),
                 make_option("--print", "Print resolved directory without changing to it"),
                 make_option("-f", "Print file-backed target path (implies --print)"),
                 make_option("--file", "Print file-backed target path (implies --print)"),
                 make_subcommand("config", "Go to ~/.config/cjsh (default)"),
                 make_subcommand("cache", "Go to ~/.cache/cjsh"),
                 make_subcommand("history", "Go to directory containing history file"),
                 make_subcommand("firstboot", "Go to directory containing first-boot marker"),
                 make_subcommand("first_boot", "Alias for firstboot"),
                 make_subcommand("completions", "Go to generated completion cache"),
                 make_subcommand("env", "Go to directory containing ~/.cjshenv or $CJSH_ENV"),
                 make_subcommand("cjshenv", "Alias for env"),
                 make_subcommand("profile", "Go to directory containing ~/.cjprofile"),
                 make_subcommand("cjprofile", "Alias for profile"),
                 make_subcommand("rc", "Go to directory containing ~/.cjshrc"),
                 make_subcommand("cjshrc", "Alias for rc"),
                 make_subcommand("logout", "Go to directory containing ~/.cjlogout"),
                 make_subcommand("cjlogout", "Alias for logout"),
                 make_subcommand("home", "Go to the HOME directory"),
                 make_subcommand("cjsh", "Go to the current cjsh executable directory")});
        add_doc("pushd", "Push the current directory onto a stack", {});
        add_doc("popd", "Pop the top directory from the stack", {});
        add_doc("dirs", "Display the directory stack", {});
        add_doc("pwd", "Print the current working directory",
                {make_option("-L", "Use logical path from PWD"),
                 make_option("--logical", "Use logical path from PWD"),
                 make_option("-P", "Resolve the physical path"),
                 make_option("--physical", "Resolve the physical path"),
                 make_option("--version", "Show version information")});

        add_doc("echo", "Write arguments to standard output",
                {make_option("-n", "Suppress trailing newline"),
                 make_option("-e", "Enable backslash escapes"),
                 make_option("-E", "Disable backslash escapes")});
        add_doc("printf", "Format and print data", {});

        add_doc("true", "Exit with a zero status", {});
        add_doc("false", "Exit with a non-zero status", {});
        add_doc(":", "No-op that always succeeds", {});

        add_doc(
            "local", "Declare variables local to the current function",
            {make_option("-a", "Declare indexed arrays"),
             make_option("-A", "Declare associative arrays"), make_option("-n", "Declare namerefs"),
             make_option("-r", "Mark names readonly"), make_option("-x", "Mark names exported")});
        add_doc(
            "declare", "Set variable attributes and values",
            {make_option("-a", "Declare indexed arrays"),
             make_option("-A", "Declare associative arrays"), make_option("-n", "Declare namerefs"),
             make_option("-f", "Operate on shell functions"),
             make_option("-F", "List function names"),
             make_option("-g", "Force global scope inside functions"),
             make_option("-p", "Print declarations"), make_option("-r", "Mark names readonly"),
             make_option("-x", "Mark names exported"),
             make_option("+x", "Remove export attribute")});
        add_alias("typeset", "declare");
        add_doc("coproc", "Run a command asynchronously with a two-way pipe", {});
        add_doc("export", "Export environment variables",
                {make_option("-p", "Print exported variables in reusable form")});
        add_doc("unset", "Remove variables from the environment",
                {make_option("-n", "Unset the nameref attribute"),
                 make_option("-v", "Select variables")});
        add_doc("set", "Configure shell options or positional parameters",
                {make_option("-e", "Exit immediately on errors"),
                 make_option("+e", "Disable exit-on-error"),
                 make_option("-C", "Enable noclobber"),
                 make_option("+C", "Disable noclobber"),
                 make_option("-u", "Treat unset variables as errors"),
                 make_option("+u", "Allow unset variables"),
                 make_option("-x", "Print commands before execution"),
                 make_option("+x", "Stop printing commands"),
                 make_option("-v", "Print shell input lines"),
                 make_option("+v", "Stop printing input lines"),
                 make_option("-n", "Read commands without executing"),
                 make_option("+n", "Resume executing commands"),
                 make_option("-f", "Disable pathname expansion"),
                 make_option("+f", "Enable pathname expansion"),
                 make_option("-a", "Auto-export modified variables"),
                 make_option("+a", "Stop auto-exporting variables"),
                 make_option("-o", "Set option by name"),
                 make_option("+o", "Unset option by name"),
                 make_option("globstar", "Enable recursive '**' glob expansion"),
                 make_option("huponexit", "Send SIGHUP to jobs when the shell exits"),
                 make_option("pipefail", "Return the last non-zero pipeline status"),
                 make_option("--errexit-severity=", "Set errexit sensitivity level"),
                 make_option("--", "Treat remaining arguments as positional parameters")});

        add_doc("shift", "Rotate positional parameters", {});

        add_doc("source", "Execute commands from a file in the current shell", {});
        add_alias(".", "source");

        add_doc("help", "Display the builtin command reference", {});
        add_doc("version", "Show cjsh version information",
                {make_option("-a", "Show extended build details"),
                 make_option("--all", "Show extended build details"),
                 make_option("--tag", "Print version tag (vX.Y.Z)"),
                 make_option("--build-time", "Print build timestamp"),
                 make_option("--compiler", "Print compiler and version"),
                 make_option("--cpp-standard", "Print C++ standard level"),
                 make_option("--cxx-standard", "Alias for --cpp-standard"),
                 make_option("--git-hash", "Print short git hash"),
                 make_option("--git-hash-full", "Print full git hash"),
                 make_option("--build-type", "Print build configuration"),
                 make_option("--arch", "Print target architecture"),
                 make_option("--platform", "Print target platform")});
        add_doc("eval", "Evaluate arguments as shell code", {});
        add_doc("if", "Evaluate a conditional block", {});
        add_doc("then", "Start the body of an if or elif branch", {});
        add_doc("elif", "Add an additional conditional branch", {});
        add_doc("else", "Provide the fallback branch for an if block", {});
        add_doc("fi", "Close an if/elif/else block", {});
        add_doc("case", "Match a word against multiple patterns", {});
        add_doc("esac", "Terminate the current case block", {});
        add_doc("for", "Iterate over each word in a list", {});
        add_doc("select", "Build an interactive menu over a list", {});
        add_doc("while", "Loop while a command succeeds", {});
        add_doc("until", "Loop until a command succeeds", {});
        add_doc("do", "Begin a loop body", {});
        add_doc("done", "End the current loop body", {});
        add_doc("function", "Define a named shell function", {});

        add_doc("history", "Show command history", {});
        add_doc("fc", "Edit or list commands from history",
                {make_option("-e", "Select editor for editing"),
                 make_option("-l", "List matching commands"),
                 make_option("-n", "Suppress line numbers when listing"),
                 make_option("-r", "Reverse the order when listing"),
                 make_option("-s", "Re-execute with substitution"),
                 make_option("-c", "Edit the provided string"),
                 make_option("--command", "Edit the provided string")});

        add_doc("exit", "Exit the shell with an optional status", {});
        add_alias("quit", "exit");
        add_alias("bye", "exit");
        add_doc("restart", "Re-exec cjsh in place",
                {make_option("-n", "Restart as plain cjsh without original startup arguments"),
                 make_option("--no-flags",
                             "Restart as plain cjsh without original startup arguments")});
        if (cjsh_filesystem::is_first_boot()) {
            add_doc("firstboot", "Suppress the welcome banner by creating its marker", {});
        }

        add_doc("test", "Evaluate conditional expressions", {});
        add_alias("[", "test");
        add_doc("[[", "Evaluate extended conditional expressions", {});

        add_doc("exec", "Replace the shell with another program", {});

        add_doc("command", "Run a command bypassing functions",
                {make_option("-p", "Use a default PATH"),
                 make_option("-v", "Print a short description"),
                 make_option("-V", "Print a verbose description"),
                 make_option("--", "Stop processing options")});

        add_doc(
            "trap", "Set or list signal handlers",
            {make_option("-l", "List available signals"), make_option("-p", "Show current traps")});

        add_doc("jobs", "List background jobs",
                {make_option("-l", "Show process-group leaders and status"),
                 make_option("-p", "Print process-group leaders only"),
                 make_option("-r", "Show running jobs only"),
                 make_option("-s", "Show stopped jobs only")});
        add_doc("jobname", "Assign a temporary display name to a job",
                {make_option("-c", "Clear any custom job name"),
                 make_option("--clear", "Clear any custom job name")});
        add_doc("fg", "Bring a job to the foreground", {});
        add_doc("bg", "Resume a job in the background", {});
        add_doc("suspend", "Suspend the current interactive shell",
                {make_option("-f", "Allow suspending a login shell")});
        add_doc("wait", "Wait for jobs or processes to finish",
                {make_option("-n", "Wait for the next job to change state"),
                 make_option("-f", "Wait for termination instead of a stop"),
                 make_value_option("-p", {}, "Store the waited job ID", ValueRequirement::Required,
                                   ValueType::Text, "VARNAME")});
        add_doc(
            "disown", "Remove jobs from the shell's management",
            {make_option("-a", "Select every job"), make_option("-r", "Select running jobs only"),
             make_option("-h", "Keep jobs but suppress SIGHUP"),
             make_option("--all", "Select every job"),
             make_option("--running", "Select running jobs only")});

        add_doc("readonly", "Mark variables as read-only",
                {make_option("-p", "Print current readonly variables"),
                 make_option("-f", "Operate on functions")});

        add_doc("read", "Read a line from standard input",
                {make_option("-r", "Disable backslash escapes"),
                 make_option("-n", "Read a specific number of characters"),
                 make_option("-u", "Read from a file descriptor"),
                 make_option("-p", "Display a prompt"), make_option("-d", "Use a custom delimiter"),
                 make_option("-t", "Set a timeout in seconds")});

        add_doc("umask", "Set or display the file mode creation mask",
                {make_option("-p", "Print in reusable format"),
                 make_option("-S", "Display the mask symbolically")});

        add_doc("ulimit", "Display or set resource limits",
                {make_option("-a", "Show all current limits"),
                 make_option("-H", "Use hard limits"),
                 make_option("-S", "Use soft limits"),
                 make_option("-c", "Limit core file size"),
                 make_option("-d", "Limit data segment size"),
                 make_option("-f", "Limit file size"),
                 make_option("-l", "Limit locked-in-memory size"),
                 make_option("-m", "Limit resident set size"),
                 make_option("-n", "Limit open file descriptors"),
                 make_option("-p", "Limit pipe buffer size"),
                 make_option("-q", "Limit POSIX message queue bytes"),
                 make_option("-r", "Limit realtime priority"),
                 make_option("-s", "Limit stack size"),
                 make_option("-t", "Limit CPU time"),
                 make_option("-u", "Limit user processes"),
                 make_option("-v", "Limit virtual memory"),
                 make_option("-w", "Limit swap size"),
                 make_option("--all", "Show all current limits"),
                 make_option("--hard", "Use hard limits"),
                 make_option("--soft", "Use soft limits")});

        add_doc("getopts", "Parse positional parameters as options", {});
        add_doc("times", "Display accumulated process times", {});

        add_doc(
            "type", "Describe how commands are resolved",
            {make_option("-a", "Show all possible resolutions"),
             make_option("-f", "Force ignoring shell functions"),
             make_option("-p", "Force PATH lookup"), make_option("-t", "Print the type keyword"),
             make_option("-P", "Force PATH lookup, ignoring functions"),
             make_option("--", "Stop processing options")});

        add_doc("which", "Locate commands in PATH",
                {make_option("-a", "Show all matches"), make_option("-s", "Silent mode"),
                 make_option("--", "Stop processing options")});

        add_doc("hash", "Manage the command lookup cache",
                {make_option("-r", "Reset cached entries")});

        add_doc("generate-completions", "Regenerate cached external completions",
                {make_option("--quiet", "Suppress per-command output"),
                 make_option("-q", "Suppress per-command output"),
                 make_option("--force", "Force regeneration even if cached"),
                 make_option("-f", "Force regeneration even if cached"),
                 make_option("--no-force", "Reuse existing cache entries"),
                 make_option("--subcommands", "Also generate discovered subcommand caches"),
                 make_option("-s", "Also generate discovered subcommand caches"),
                 make_value_option("--jobs", {"-j"}, "Set the number of parallel jobs",
                                   ValueRequirement::Required, ValueType::Text, "JOBS",
                                   ValueSeparator::Either),
                 make_option("--", "Treat remaining arguments as command names"),
                 make_positional("COMMAND", "Command to generate completions for",
                                 ValueType::Command, 1, true)});

        add_doc("hook", "Manage shell lifecycle hooks",
                {make_subcommand("add", "Register a function for a hook"),
                 make_subcommand("remove", "Unregister a function"),
                 make_subcommand("list", "Show registered hooks"),
                 make_subcommand("clear", "Remove hooks for a type")});

        add_doc("hook-add", "",
                {make_subcommand("precmd", "Run before the prompt"),
                 make_subcommand("preexec", "Run before executing commands"),
                 make_subcommand("chpwd", "Run after changing directories"),
                 make_subcommand("idle", "Run after terminal inactivity")});
        add_doc("hook-remove", "",
                {make_subcommand("precmd", "Run before the prompt"),
                 make_subcommand("preexec", "Run before executing commands"),
                 make_subcommand("chpwd", "Run after changing directories"),
                 make_subcommand("idle", "Run after terminal inactivity")});
        add_doc("hook-clear", "",
                {make_subcommand("precmd", "Run before the prompt"),
                 make_subcommand("preexec", "Run before executing commands"),
                 make_subcommand("chpwd", "Run after changing directories"),
                 make_subcommand("idle", "Run after terminal inactivity")});
        add_doc("hook-list", "",
                {make_subcommand("precmd", "Run before the prompt"),
                 make_subcommand("preexec", "Run before executing commands"),
                 make_subcommand("chpwd", "Run after changing directories"),
                 make_subcommand("idle", "Run after terminal inactivity")});

        add_doc("builtin", "Invoke a builtin bypassing functions", {});

        add_doc("cjsh-widget", "Invoke an interactive widget",
                {make_subcommand("get-buffer", "Print the current input buffer"),
                 make_subcommand("set-buffer", "Replace the input buffer content"),
                 make_subcommand("get-cursor", "Show the cursor position"),
                 make_subcommand("set-cursor", "Move the cursor to a byte offset"),
                 make_subcommand("insert", "Insert text at the cursor"),
                 make_subcommand("append", "Append text to the buffer"),
                 make_subcommand("clear", "Clear the input buffer"),
                 make_subcommand("action", "Execute a built-in editor action"),
                 make_subcommand("accept", "Accept and submit the current buffer")});

        add_doc(
            "cjshopt", "Configure cjsh interactive behavior",
            {make_subcommand("style_def", "Define syntax highlight styles"),
             make_subcommand("completion-case", "Configure completion case sensitivity"),
             make_subcommand("history-search-case", "Configure fuzzy history case sensitivity"),
             make_subcommand("history-directory", "Scope history to the current directory"),
             make_subcommand("history-directory-subdirs", "Include nested directories in history"),
             make_subcommand("completion-spell", "Configure completion spell correction"),
             make_subcommand("completion-spell-enter",
                             "Auto-apply a single spell correction when pressing Enter"),
             make_subcommand("completion-learning", "Toggle completion learning"),
             make_subcommand("exit-confirmation", "Control when exit requires confirmation"),
             make_subcommand("smart-cd", "Toggle smart cd auto-jumps"),
             make_subcommand("extglob", "Toggle extended glob patterns"),
             make_subcommand("script-extension-interpreter",
                             "Toggle extension-based script runners"),
             make_subcommand("line-numbers", "Configure multiline line numbers"),
             make_subcommand("line-numbers-replace-prompt",
                             "Replace the final prompt line with line numbers"),
             make_subcommand("line-numbers-continuation",
                             "Control line numbers during continuation prompts"),

             make_subcommand("current-line-number-highlight",
                             "Toggle current line number highlighting"),
             make_subcommand("multiline-start-lines", "Set default multiline prompt height"),
             make_subcommand("multiline-max-lines", "Limit visible multiline input rows"),
             make_subcommand("menu-max-lines", "Limit visible menu content rows"),
             make_subcommand("multiline-bottom-lines", "Set the input and menu scroll margin"),

             make_subcommand("hint-delay", "Adjust inline hint delay"),
             make_subcommand("idle-timeout", "Configure the idle hook timeout"),
             make_subcommand("completion-preview", "Toggle completion preview"),
             make_subcommand("completion-click-accept",
                             "Control whether clicks accept completion entries"),
             make_subcommand("menu-highlighting",
                             "Syntax-highlight completion and history menu items"),
             make_subcommand("visible-whitespace", "Toggle visible whitespace"),
             make_subcommand("line-wrap-marker", "Set the character at wrapped line ends"),
             make_subcommand("hint", "Toggle inline hints"),
             make_subcommand("multiline-indent", "Toggle multiline auto-indent"),
             make_subcommand("multiline", "Toggle multiline input"),
             make_subcommand("inline-help", "Toggle inline help"),
             make_subcommand("status-hints", "Control status hint visibility"),
             make_subcommand("status-line", "Disable the status row entirely"),
             make_subcommand("status-reporting", "Mute cjsh status messages"),
             make_subcommand("status-line-callback",
                             "Run a shell function to publish status-line text"),
             make_subcommand("mouse-clicking", "Configure prompt and menu mouse capture"),
             make_subcommand("mouse-clicking-status-line",
                             "Toggle the mouse-clicking status indicator line"),
             make_subcommand("auto-tab", "Toggle automatic tab completion"),
             make_subcommand("prompt-newline", "Toggle newline after command execution"),
             make_subcommand("right-prompt-follow-cursor", "Move the right prompt with the cursor"),
             make_subcommand("agent-mode", "Configure agent-assisted command writing"),
             make_subcommand("keybind", "Inspect or modify key bindings"),
             make_subcommand("generate-profile", "Generate ~/.cjprofile"),
             make_subcommand("generate-env", "Generate ~/.cjshenv"),
             make_subcommand("generate-rc", "Generate ~/.cjshrc"),
             make_subcommand("generate-logout", "Generate ~/.cjlogout"),
             make_subcommand("set-history-max", "Configure history persistence"),
             make_subcommand("set-completion-max", "Limit completion suggestions")});

        add_doc("cjshopt-style_def", "Define or reset syntax styles",
                {make_subcommand("preview", "Show current syntax style preview"),
                 make_option("--reset", "Reset all highlight styles to defaults")});

        add_doc("cjshopt-completion-learning", "Toggle completion learning",
                {make_subcommand("on", "Allow on-demand completion scraping"),
                 make_subcommand("off", "Only use cached completions"),
                 make_subcommand("status", "Show current setting")});

        add_doc("cjshopt-exit-confirmation", "Control exit confirmation",
                {make_subcommand("smart", "Confirm only when running or stopped jobs exist"),
                 make_subcommand("always", "Confirm every exit request"),
                 make_subcommand("never", "Never confirm exit requests"),
                 make_subcommand("status", "Show current mode")});

        add_doc("cjshopt-hint-delay", "Adjust inline hint delay",
                {make_subcommand("status", "Show the current delay in milliseconds"),
                 make_option("--status", "Show the current delay in milliseconds")});

        add_doc("cjshopt-idle-timeout", "Configure terminal inactivity hooks",
                {make_subcommand("off", "Disable idle hooks"),
                 make_subcommand("status", "Show the current timeout"),
                 make_option("--status", "Show the current timeout")});

        add_doc("cjshopt-set-history-max", "Configure history persistence",
                {make_subcommand("default", "Restore the default history limit"),
                 make_option("--default", "Restore the default history limit"),
                 make_subcommand("status", "Display the current history limit"),
                 make_option("--status", "Display the current history limit")});

        add_doc("cjshopt-set-completion-max", "Limit completion suggestions",
                {make_subcommand("default", "Restore the default completion limit"),
                 make_option("--default", "Restore the default completion limit"),
                 make_subcommand("status", "Display the current completion limit"),
                 make_option("--status", "Display the current completion limit")});

        add_doc("cjshopt-keybind", "Inspect or modify key bindings",
                {make_subcommand("list", "Show current key bindings"),
                 make_subcommand("set", "Replace bindings for an action"),
                 make_subcommand("add", "Add bindings for an action"),
                 make_subcommand("clear", "Remove bindings for key sequences"),
                 make_subcommand("clear-action", "Remove bindings for an action"),
                 make_subcommand("reset", "Restore default key bindings"),
                 make_subcommand("profile", "Manage key binding profiles"),
                 make_subcommand("ext", "Manage command key bindings")});

        add_doc("cjshopt-agent-mode", "Configure agent-assisted command writing",
                {make_subcommand("set", "Add or replace an agent executor"),
                 make_subcommand("list", "Show configured executors"),
                 make_subcommand("status", "Show agent-mode status"),
                 make_subcommand("on", "Enable agent command writing"),
                 make_subcommand("off", "Disable agent command writing"),
                 make_subcommand("key", "Configure the activation key"),
                 make_subcommand("clear", "Remove executor configuration"),
                 make_subcommand("reset", "Restore agent-mode defaults")});

        add_doc("cjshopt-agent-mode-set", "Add or replace an agent executor",
                {make_option("--command", "Executor command; the request is its final argument"),
                 make_option("--system-prompt", "Guidance added after CJSH's protocol prompt"),
                 make_option("--trigger-prefix", "Input prefix that selects this executor")});

        add_doc("cjshopt-agent-mode-key", "Configure the agent activation key",
                {make_subcommand("default", "Restore the default Alt+A binding"),
                 make_subcommand("off", "Disable direct key activation"),
                 make_subcommand("status", "Show the activation key")});

        add_doc("cjshopt-agent-mode-clear", "Remove agent executor configuration",
                {make_option("--default", "Remove the fallback executor"),
                 make_option("--trigger-prefix", "Remove the executor for a prefix"),
                 make_option("--all", "Remove every executor")});

        add_doc("cjshopt-keybind-profile", "Manage key binding profiles",
                {make_subcommand("list", "List available key binding profiles"),
                 make_subcommand("set", "Activate a key binding profile")});

        add_doc("cjshopt-keybind-ext", "Manage custom command key bindings",
                {make_subcommand("list", "Show custom command key bindings"),
                 make_subcommand("set", "Bind a key to a shell command"),
                 make_subcommand("clear", "Remove custom command key bindings"),
                 make_subcommand("reset", "Clear all custom command key bindings")});

        add_doc("cjshopt-generate-profile", "Generate ~/.cjprofile",
                {make_option("--force", "Overwrite the existing profile"),
                 make_option("-f", "Overwrite the existing profile"),
                 make_option("--alt", "Write to the alternate configuration path"),
                 make_option("--help", "Show usage information"),
                 make_option("-h", "Show usage information")});

        add_doc("cjshopt-generate-env", "Generate ~/.cjshenv",
                {make_option("--force", "Overwrite the existing env file"),
                 make_option("-f", "Overwrite the existing env file"),
                 make_option("--alt", "Write to the alternate configuration path"),
                 make_option("--help", "Show usage information"),
                 make_option("-h", "Show usage information")});

        add_doc("cjshopt-generate-rc", "Generate ~/.cjshrc",
                {make_option("--force", "Overwrite the existing rc file"),
                 make_option("-f", "Overwrite the existing rc file"),
                 make_option("--alt", "Write to the alternate configuration path"),
                 make_option("--help", "Show usage information"),
                 make_option("-h", "Show usage information")});

        add_doc("cjshopt-generate-logout", "Generate ~/.cjlogout",
                {make_option("--force", "Overwrite the existing logout file"),
                 make_option("-f", "Overwrite the existing logout file"),
                 make_option("--alt", "Write to the alternate configuration path"),
                 make_option("--help", "Show usage information"),
                 make_option("-h", "Show usage information")});

        add_doc("cjshopt-completion-case", "",
                {make_subcommand("on", "Enable case-sensitive matches"),
                 make_subcommand("off", "Disable case sensitivity"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-history-search-case", "",
                {make_subcommand("on", "Require exact case in fuzzy history"),
                 make_subcommand("off", "Match history regardless of case"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-history-directory", "",
                {make_subcommand("on", "Scope history to the current directory"),
                 make_subcommand("off", "Recall history from all directories"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-history-directory-subdirs", "",
                {make_subcommand("on", "Include commands from nested directories"),
                 make_subcommand("off", "Match only the current directory"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-completion-spell", "",
                {make_subcommand("on", "Enable spell correction"),
                 make_subcommand("off", "Disable spell correction"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-completion-spell-enter", "",
                {make_subcommand("on", "Auto-apply a single spell correction on Enter"),
                 make_subcommand("off", "Submit input without Enter autocorrection"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-smart-cd", "",
                {make_subcommand("on", "Enable smart cd auto-jumps"),
                 make_subcommand("off", "Disable smart cd auto-jumps"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-extglob", "",
                {make_subcommand("on", "Enable extended glob patterns"),
                 make_subcommand("off", "Disable extended glob patterns"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-script-extension-interpreter", "",
                {make_subcommand("on", "Enable extension-based script runners"),
                 make_subcommand("off", "Disable extension-based script runners"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-completion-preview", "",
                {make_subcommand("on", "Enable completion preview"),
                 make_subcommand("off", "Disable completion preview"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-completion-click-accept", "",
                {make_subcommand("on", "Always accept completion entries on click"),
                 make_subcommand("off", "Click selects entries without accepting"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-menu-highlighting", "",
                {make_subcommand("none", "Do not syntax-highlight menu items"),
                 make_subcommand("single", "Highlight only the selected menu item"),
                 make_subcommand("all", "Highlight every rendered menu item"),
                 make_subcommand("reverse",
                                 "Highlight every rendered menu item except the selected item"),
                 make_subcommand("status", "Show current mode")});
        add_doc("cjshopt-visible-whitespace", "",
                {make_subcommand("on", "Show whitespace markers"),
                 make_subcommand("off", "Hide whitespace markers"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-line-wrap-marker", "",
                {make_subcommand("''", "Hide the symbol at wrapped line ends"),
                 make_subcommand("status", "Show the current marker")});
        add_doc("cjshopt-hint", "",
                {make_subcommand("on", "Enable inline hints"),
                 make_subcommand("off", "Disable inline hints"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-multiline-indent", "",
                {make_subcommand("on", "Enable multiline auto-indent"),
                 make_subcommand("off", "Disable multiline auto-indent"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-multiline", "",
                {make_subcommand("on", "Enable multiline input"),
                 make_subcommand("off", "Disable multiline input"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-inline-help", "",
                {make_subcommand("on", "Enable inline help"),
                 make_subcommand("off", "Disable inline help"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-status-hints", "",
                {make_subcommand("off", "Never display the hint banner"),
                 make_subcommand("normal", "Only show when everything else is blank"),
                 make_subcommand("transient", "Show when the status line has no content"),
                 make_subcommand("persistent", "Always prepend hints above other lines"),
                 make_subcommand("status", "Show current mode")});
        add_doc("cjshopt-status-line", "",
                {make_subcommand("on", "Show the status row"),
                 make_subcommand("off", "Hide the status row entirely"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-status-reporting", "",
                {make_subcommand("on", "Show cjsh validation output"),
                 make_subcommand("off", "Hide cjsh validation output"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-status-line-callback", "",
                {make_subcommand("off", "Disable custom status-line callback output"),
                 make_subcommand("status", "Show current callback setting")});
        add_doc("cjshopt-mouse-clicking", "",
                {make_subcommand("all-off", "Never capture mouse events, including in menus"),
                 make_subcommand("off", "Capture only in expanded/interactive menus"),
                 make_subcommand("simple", "Enable mouse capture with manual toggles"),
                 make_subcommand("smart", "Enable auto suspend/resume mouse handling"),
                 make_subcommand("on", "Alias for simple mode"),
                 make_subcommand("disabled", "Alias for all-off mode"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-mouse-clicking-status-line", "",
                {make_subcommand("on", "Show the mouse-clicking status indicator"),
                 make_subcommand("off", "Hide the mouse-clicking status indicator"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-auto-tab", "",
                {make_subcommand("on", "Enable automatic tab completion"),
                 make_subcommand("off", "Disable automatic tab completion"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-line-numbers", "",
                {make_subcommand("on", "Enable absolute line numbers"),
                 make_subcommand("off", "Hide line numbers"),
                 make_subcommand("relative", "Show relative line numbers"),
                 make_subcommand("absolute", "Show absolute line numbers"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-line-numbers-replace-prompt", "",
                {make_subcommand("on", "Replace the final prompt line with line numbers"),
                 make_subcommand("off", "Keep the final prompt line visible"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-line-numbers-continuation", "",
                {make_subcommand("on", "Show line numbers with continuation prompts"),
                 make_subcommand("off", "Hide line numbers when a continuation prompt is active"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-current-line-number-highlight", "",
                {make_subcommand("on", "Highlight the active line number"),
                 make_subcommand("off", "Disable line number highlighting"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-prompt-newline", "",
                {make_subcommand("on", "Insert a newline after every command"),
                 make_subcommand("off", "Skip the post-command newline"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-right-prompt-follow-cursor", "",
                {make_subcommand("on", "Move the right prompt with the cursor"),
                 make_subcommand("off", "Keep the right prompt pinned to the first row"),
                 make_subcommand("status", "Show current setting")});
        add_doc("cjshopt-multiline-start-lines", "",
                {make_subcommand("status", "Show current multiline height")});
        add_doc("cjshopt-multiline-max-lines", "",
                {make_subcommand("status", "Show the multiline viewport limit")});
        add_doc("cjshopt-menu-max-lines", "Limit visible menu content rows",
                {make_subcommand("status", "Show the menu height limit")});
        add_doc("cjshopt-multiline-bottom-lines", "",
                {make_subcommand("status", "Show the cursor scroll margin")});
        return map;
    }();
    return docs;
}

}  // namespace

const CommandDoc* lookup_builtin_command_doc(const std::string& doc_target) {
    if (doc_target == "firstboot" && !cjsh_filesystem::is_first_boot()) {
        return nullptr;
    }

    if (const auto* dynamic_doc = lookup_dynamic_builtin_doc(doc_target)) {
        return dynamic_doc;
    }

    const auto& docs = builtin_command_docs();
    auto it = docs.find(doc_target);
    if (it != docs.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string get_builtin_summary(const std::string& command) {
    if (const auto* doc = lookup_builtin_command_doc(command)) {
        return doc->summary;
    }
    return {};
}

}  // namespace builtin_completions
