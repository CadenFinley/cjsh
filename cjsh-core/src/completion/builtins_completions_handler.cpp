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
#include "cjshopt_registry.h"
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
            {make_option("-h", "Enable command hashing"),
             make_option("--help", "Display help message and exit"),
             make_option("-v", "Print shell input lines"),
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
             make_value_option("--dialect", {}, "Select cjsh or posix", ValueRequirement::Required,
                               ValueType::Text, "DIALECT"),
             make_option("-m", "Enable job-control monitor mode"),
             make_option("--minimal", "Disable cjsh enhancements"),
             make_option("-C", "Enable noclobber"),
             make_option("--no-colors", "Disable color output"),
             make_option("-N", "Skip sourcing ~/.cjshrc"),
             make_option("--no-source", "Skip sourcing ~/.cjshrc"),
             make_option("-O", "Enable a shopt option"),
             make_option("--no-completions", "Disable tab completions"),
             make_option("--no-completion-learning", "Disable on-demand completion learning"),
             make_option("--no-script-extension-interpreter",
                         "Disable extension-based script runners"),
             make_option("--no-smart-cd", "Disable smart cd auto-jumps"),
             make_option("-S", "Disable syntax highlighting"),
             make_option("--no-syntax-highlighting", "Disable syntax highlighting"),
             make_option("--no-error-suggestions", "Disable error suggestions"),
             make_option("--no-agent", "Disable agent assistance"),
             make_option("--no-prompt-vars", "Ignore PS1/PS2 prompt variables"),
             make_option("--no-history", "Disable history recording and history expansion"),
             make_option("-H", "Enable history expansion"),
             make_option("--no-history-expansion", "Disable history expansion (!commands)"),
             make_option("-W", "Suppress the sh invocation warning"),
             make_option("--no-sh-warning", "Suppress the sh invocation warning"),
             make_option("-L", "Disable title line on startup"),
             make_option("--no-titleline", "Disable title line on startup"),
             make_option("-U", "Display startup time"),
             make_option("--show-startup-time", "Display startup time"),
             make_option("-s", "Read commands from standard input"),
             make_option("--secure", "Secure mode: disable cjshenv/profile/rc/logout files")});

        add_doc(
            "shopt", "Inspect or change shell language options",
            {make_option("-s", "Enable named options"), make_option("-u", "Disable named options"),
             make_option("-q", "Query without output"),
             make_option("-p", "Print reusable commands"),
             make_option("-o", "Use the set option namespace")});

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

        std::vector<CompletionEntry> cjshopt_entries;
        cjshopt_entries.reserve(cjshopt_subcommands().size());
        for (const auto& command : cjshopt_subcommands()) {
            cjshopt_entries.push_back(make_subcommand(command.name, command.summary));
            std::vector<CompletionEntry> values;
            values.reserve(command.values.size());
            for (const auto& value : command.values) {
                values.push_back(value.option ? make_option(value.text, value.description)
                                              : make_subcommand(value.text, value.description));
            }
            add_doc("cjshopt-" + std::string(command.name), command.value_summary,
                    std::move(values));
        }
        add_doc("cjshopt", "Configure cjsh interactive behavior", std::move(cjshopt_entries));

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
