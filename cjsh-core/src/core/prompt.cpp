/*
  prompt.cpp

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

#include "prompt.h"
#include <sys/types.h>

#include <pwd.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cjsh_filesystem.h"
#include "exec.h"
#include "flags.h"
#include "isocline.h"
#include "job_control.h"
#include "keycodes.h"
#include "numeric_utils.h"
#include "parser.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "string_utils.h"
#include "token_constants.h"
#include "version_command.h"

namespace prompt {

namespace {

std::atomic<bool> g_prompt_refresh_allowed{false};
thread_local std::string g_transient_final_right_prompt;

}  // namespace
namespace {

enum class PromptContext : uint8_t {
    Primary,
    Right,
    Secondary,
    Final,
};

struct GitDirInfo {
    std::filesystem::path root;
    std::filesystem::path git_dir;
};

struct GitRepositoryContext {
    std::filesystem::path workdir;
    std::filesystem::path root;
    std::filesystem::path git_dir;
};

std::string read_git_head(const std::filesystem::path& git_dir);

struct GitStatusSnapshot {
    std::filesystem::file_time_type index_mtime;
    std::filesystem::file_time_type worktree_mtime;

    bool equals(const GitStatusSnapshot& other) const {
        return index_mtime == other.index_mtime && worktree_mtime == other.worktree_mtime;
    }
};

std::filesystem::file_time_type safe_last_write_time(const std::filesystem::path& path) {
    std::error_code ec;
    auto value = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    return value;
}

GitStatusSnapshot capture_git_status_snapshot(const GitRepositoryContext& ctx) {
    GitStatusSnapshot snapshot;
    snapshot.index_mtime = safe_last_write_time(ctx.git_dir / "index");
    snapshot.worktree_mtime = safe_last_write_time(ctx.root);
    return snapshot;
}

std::string format_git_branch_segment(const std::string& branch_name) {
    if (branch_name.empty()) {
        return {};
    }
    std::string segment;
    segment.reserve(branch_name.size() + 32);
    segment += "[b color=ansi-blue]git:([/]";
    segment += "[color=#ff6b6b]";
    segment += branch_name;
    segment += "[/]";
    segment += "[color=ansi-blue])[/]";
    return segment;
}

std::string format_git_status_segment(bool dirty) {
    if (!dirty) {
        return {};
    }
    return " [color=#ffd166]✗[/color]";
}

constexpr std::chrono::milliseconds kGitStatusCacheTtl(1500);

class GitPromptCache {
   public:
    std::string branch_segment(const GitRepositoryContext& ctx) {
        const std::string key = cache_key(ctx);
        const auto head_mtime = safe_last_write_time(ctx.git_dir / "HEAD");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& entry = entries_[key];
            if (entry.branch_valid && entry.head_mtime == head_mtime) {
                return entry.branch_segment;
            }
        }

        std::string branch_name = read_git_head(ctx.git_dir);
        std::string formatted = format_git_branch_segment(branch_name);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& entry = entries_[key];
            entry.branch_segment = formatted;
            entry.head_mtime = head_mtime;
            entry.branch_valid = true;
        }

        return formatted;
    }

    std::optional<std::string> status_segment_if_fresh(const GitRepositoryContext& ctx,
                                                       const GitStatusSnapshot& snapshot,
                                                       std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[cache_key(ctx)];
        if (entry.status_valid && entry.status_snapshot.equals(snapshot) &&
            now - entry.last_status_refresh <= kGitStatusCacheTtl) {
            return entry.status_segment;
        }
        return std::nullopt;
    }

    void store_status_result(const GitRepositoryContext& ctx, bool dirty,
                             const GitStatusSnapshot& snapshot,
                             std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[cache_key(ctx)];
        entry.status_snapshot = snapshot;
        entry.status_segment = format_git_status_segment(dirty);
        entry.status_valid = true;
        entry.last_status_refresh = now;
    }

   private:
    struct CacheEntry {
        std::filesystem::file_time_type head_mtime;
        bool branch_valid = false;
        std::string branch_segment;

        GitStatusSnapshot status_snapshot{};
        bool status_valid = false;
        std::chrono::steady_clock::time_point last_status_refresh;
        std::string status_segment;
    };

    static std::string cache_key(const GitRepositoryContext& ctx) {
        return ctx.root.lexically_normal().string();
    }

    std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> entries_;
};

GitPromptCache& git_prompt_cache() {
    static GitPromptCache cache;
    return cache;
}

std::string get_env(const char* name, const char* fallback = nullptr) {
    std::string value = cjsh_env::get_shell_variable_value(name);
    if (!value.empty()) {
        return value;
    }
    if (fallback != nullptr) {
        return fallback;
    }
    return {};
}

std::string get_exit_status();

std::string get_shell_name() {
    std::string shell_name = get_env("0");
    if (shell_name.empty() && !flags::startup_args().empty()) {
        shell_name = flags::startup_args().front();
    }
    if (shell_name.empty()) {
        shell_name = "cjsh";
    }
    auto pos = shell_name.find_last_of('/');
    if (pos != std::string::npos) {
        shell_name = shell_name.substr(pos + 1);
    }
    return shell_name;
}

std::string get_username() {
    std::string username = get_env("USER");
    if (!username.empty()) {
        return username;
    }
    uid_t uid = geteuid();
    passwd* pw = getpwuid(uid);
    if (pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
    }
    return {};
}

std::string get_hostname(bool full) {
    char buffer[256] = {0};
    if (gethostname(buffer, sizeof(buffer)) != 0) {
        return {};
    }
    std::string host(buffer);
    if (!full) {
        auto pos = host.find('.');
        if (pos != std::string::npos) {
            host.resize(pos);
        }
    }
    return host;
}

bool terminal_supports_color() {
    if (isatty(STDOUT_FILENO) == 0) {
        return false;
    }

    if (cjsh_env::shell_variable_is_set("NO_COLOR")) {
        return false;
    }

    const std::string colorterm = cjsh_env::get_shell_variable_value("COLORTERM");
    if (!colorterm.empty()) {
        std::string colorterm_lower = string_utils::to_lower_copy(colorterm);
        if (colorterm_lower.find("nocolor") != std::string::npos ||
            colorterm_lower.find("monochrome") != std::string::npos) {
            return false;
        }
        if (!colorterm_lower.empty()) {
            return true;
        }
    }

    const std::string term = cjsh_env::get_shell_variable_value("TERM");
    if (term.empty()) {
        return true;
    }

    std::string term_lower = string_utils::to_lower_copy(term);

    const bool unsupported = term_lower.find("dumb") != std::string::npos ||
                             term_lower.find("cons25") != std::string::npos ||
                             term_lower.find("emacs") != std::string::npos ||
                             term_lower.find("nocolor") != std::string::npos ||
                             term_lower.find("monochrome") != std::string::npos;

    return !unsupported;
}

std::string build_default_terminal_title() {
    std::string shell_value = "cjsh";

    std::string pwd_value = get_env("PWD");
    if (pwd_value.empty()) {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) {
            pwd_value = cwd.string();
        }
    }

    if (shell_value.empty() && pwd_value.empty()) {
        return {};
    }

    if (shell_value.empty()) {
        return pwd_value;
    }

    if (pwd_value.empty()) {
        return shell_value;
    }

    std::string title;
    title.reserve(shell_value.size() + 3 + pwd_value.size());
    (void)title.append(shell_value);
    (void)title.append(" - ");
    (void)title.append(pwd_value);
    return title;
}

std::string format_time(const char* fmt) {
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    (void)localtime_r(&now, &tm_now);

    char buffer[256];
    size_t written = std::strftime(buffer, sizeof(buffer), fmt, &tm_now);
    if (written == 0) {
        return {};
    }
    return std::string(buffer, written);
}

std::string get_cwd(bool abbreviate_home, bool basename_only) {
    return cjsh_filesystem::formatted_current_directory(abbreviate_home, basename_only);
}

std::string get_abbreviated_cwd() {
    const std::string path = get_cwd(true, false);
    std::string abbreviated;
    abbreviated.reserve(path.size());

    size_t start = 0;
    for (size_t separator = path.find('/'); separator != std::string::npos;
         separator = path.find('/', start)) {
        if (separator > start) {
            // Keep the dot on hidden directories and the first complete UTF-8 character.
            size_t initial = start;
            if (path[initial] == '.' && initial + 1 < separator) {
                ++initial;
            }
            const long next = ic_next_char(path.c_str(), static_cast<long>(initial));
            const size_t end =
                next > static_cast<long>(initial) ? static_cast<size_t>(next) : initial + 1;
            abbreviated.append(path, start, end - start);
        }
        abbreviated.push_back('/');
        start = separator + 1;
    }

    // Leave the final directory name intact (including '~' when already at home).
    abbreviated.append(path, start, std::string::npos);
    return abbreviated;
}

std::string get_terminal_name() {
    const char* tty = ttyname(STDIN_FILENO);
    if (tty == nullptr) {
        return {};
    }
    std::string name(tty);
    auto pos = name.find_last_of('/');
    if (pos != std::string::npos) {
        name = name.substr(pos + 1);
    }
    return name;
}

std::optional<std::filesystem::path> git_dir_from_worktree_file(
    const std::filesystem::path& candidate, const std::filesystem::path& base) {
    std::ifstream file(candidate);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    (void)std::getline(file, line);
    line = string_utils::trim_ascii_whitespace_copy(line);
    constexpr std::string_view kPrefix = "gitdir:";
    if (line.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    std::string path_str = string_utils::trim_ascii_whitespace_copy(line.substr(kPrefix.size()));
    if (path_str.empty()) {
        return std::nullopt;
    }
    std::filesystem::path gitdir(path_str);
    if (gitdir.is_relative()) {
        return (base / gitdir).lexically_normal();
    }
    return gitdir;
}

std::optional<GitDirInfo> locate_git_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path candidate = dir / ".git";
    if (!std::filesystem::exists(candidate, ec)) {
        return std::nullopt;
    }
    if (std::filesystem::is_directory(candidate, ec)) {
        return GitDirInfo{dir, candidate};
    }
    if (!std::filesystem::is_regular_file(candidate, ec)) {
        return std::nullopt;
    }
    if (auto gitdir = git_dir_from_worktree_file(candidate, dir)) {
        return GitDirInfo{dir, *gitdir};
    }
    return std::nullopt;
}

std::optional<GitDirInfo> find_git_directory(const std::filesystem::path& start) {
    std::filesystem::path current = start;
    while (true) {
        if (auto git_info = locate_git_directory(current)) {
            return git_info;
        }
        if (!current.has_parent_path()) {
            break;
        }
        auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::string read_git_head(const std::filesystem::path& git_dir) {
    std::ifstream head_file(git_dir / "HEAD");
    if (!head_file.is_open()) {
        return {};
    }
    std::string line;
    (void)std::getline(head_file, line);
    line = string_utils::trim_ascii_whitespace_copy(line);
    if (line.empty()) {
        return {};
    }
    constexpr std::string_view kRefPrefix = "ref:";
    if (line.rfind(kRefPrefix, 0) == 0) {
        std::string ref = string_utils::trim_ascii_whitespace_copy(line.substr(kRefPrefix.size()));
        auto slash = ref.find_last_of('/');
        if (slash != std::string::npos) {
            return ref.substr(slash + 1);
        }
        return ref;
    }
    if (line.size() > 7) {
        return line.substr(0, 7);
    }
    return line;
}

bool git_has_changes(const std::filesystem::path& workdir) {
    std::vector<std::string> args = {
        "git",    "-C",          workdir.empty() ? "." : workdir.string(),
        "status", "--porcelain", "--untracked-files=normal"};
    auto output = exec_utils::execute_command_vector_for_output(args);
    return !output.output.empty();
}

std::optional<GitRepositoryContext> detect_git_context() {
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        return std::nullopt;
    }
    auto git_info = find_git_directory(cwd);
    if (!git_info) {
        return std::nullopt;
    }
    GitRepositoryContext context;
    context.workdir = std::move(cwd);
    context.root = git_info->root;
    context.git_dir = git_info->git_dir;
    return context;
}

std::string build_git_segment(const GitRepositoryContext& ctx) {
    std::string branch_text = git_prompt_cache().branch_segment(ctx);
    if (branch_text.empty()) {
        return {};
    }

    std::string segment = branch_text;
    size_t status_insert_offset = segment.size();
    segment.push_back(' ');

    GitStatusSnapshot snapshot = capture_git_status_snapshot(ctx);
    auto now = std::chrono::steady_clock::now();
    std::string status_text;
    if (auto cached = git_prompt_cache().status_segment_if_fresh(ctx, snapshot, now)) {
        status_text = *cached;
    } else {
        bool dirty = git_has_changes(ctx.workdir);
        GitStatusSnapshot fresh_snapshot = capture_git_status_snapshot(ctx);
        git_prompt_cache().store_status_result(ctx, dirty, fresh_snapshot,
                                               std::chrono::steady_clock::now());
        status_text = format_git_status_segment(dirty);
    }

    if (!status_text.empty()) {
        (void)segment.insert(status_insert_offset, status_text);
    }

    return segment;
}

std::string render_git_segment_now() {
    auto context = detect_git_context();
    if (!context) {
        return {};
    }
    return build_git_segment(*context);
}

class AsyncGitPromptManager {
   public:
    void begin_render();
    void finalize_render(const std::string& prompt_snapshot);
    void append_segment(std::string& builder);
    std::optional<std::string> consume_ready_prompt();

   private:
    void start_worker_locked(size_t request_id, const GitRepositoryContext& context);
    void handle_worker_result(size_t request_id, std::string segment);
    void reset_async_state_locked();

    std::mutex mutex_;
    std::condition_variable cv_;
    bool async_enabled_cached_ = false;
    bool worker_running_ = false;
    size_t worker_request_id_ = 0;
    size_t request_counter_ = 0;
    size_t active_request_id_ = 0;
    size_t pending_prompt_request_id_ = 0;
    size_t result_request_id_ = 0;
    bool refresh_pending_ = false;
    bool awaiting_async_result_ = false;
    bool prompt_snapshot_ready_ = false;
    bool notify_when_snapshot_ready_ = false;
    bool result_ready_ = false;
    std::string pending_prompt_;
    std::vector<size_t> placeholder_offsets_;
    std::string pending_segment_;
};

constexpr std::chrono::milliseconds kGitInlineBudget(35);

void AsyncGitPromptManager::begin_render() {
    std::lock_guard<std::mutex> lock(mutex_);
    async_enabled_cached_ = config::interactive_mode;
    if (!async_enabled_cached_) {
        active_request_id_ = 0;
        placeholder_offsets_.clear();
        pending_prompt_.clear();
        refresh_pending_ = false;
        awaiting_async_result_ = false;
        prompt_snapshot_ready_ = false;
        notify_when_snapshot_ready_ = false;
        result_ready_ = false;
        return;
    }
    ++request_counter_;
    active_request_id_ = request_counter_;
    placeholder_offsets_.clear();
    pending_prompt_.clear();
    refresh_pending_ = false;
    awaiting_async_result_ = false;
    prompt_snapshot_ready_ = false;
    notify_when_snapshot_ready_ = false;
    result_ready_ = false;
}

void AsyncGitPromptManager::append_segment(std::string& builder) {
    if (!async_enabled_cached_) {
        builder += render_git_segment_now();
        return;
    }

    auto context = detect_git_context();
    if (!context) {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_async_state_locked();
        return;
    }

    std::string branch_text = git_prompt_cache().branch_segment(*context);
    if (branch_text.empty()) {
        return;
    }

    builder += branch_text;
    size_t status_insert_offset = builder.size();
    builder.push_back(' ');

    GitStatusSnapshot snapshot = capture_git_status_snapshot(*context);
    auto now = std::chrono::steady_clock::now();
    if (auto cached_status = git_prompt_cache().status_segment_if_fresh(*context, snapshot, now)) {
        if (!cached_status->empty()) {
            (void)builder.insert(status_insert_offset, *cached_status);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        reset_async_state_locked();
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    if (!worker_running_ || worker_request_id_ != active_request_id_) {
        start_worker_locked(active_request_id_, *context);
    }

    if (cv_.wait_for(lock, kGitInlineBudget,
                     [&] { return result_ready_ && result_request_id_ == active_request_id_; })) {
        std::string status_text = pending_segment_;
        reset_async_state_locked();
        lock.unlock();
        if (!status_text.empty()) {
            (void)builder.insert(status_insert_offset, status_text);
        }
        return;
    }

    awaiting_async_result_ = true;
    refresh_pending_ = true;
    placeholder_offsets_.push_back(status_insert_offset);
}

void AsyncGitPromptManager::finalize_render(const std::string& prompt_snapshot) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!async_enabled_cached_ || !awaiting_async_result_ || placeholder_offsets_.empty()) {
        pending_prompt_.clear();
        prompt_snapshot_ready_ = false;
        notify_when_snapshot_ready_ = false;
        return;
    }
    pending_prompt_ = prompt_snapshot;
    prompt_snapshot_ready_ = true;
    pending_prompt_request_id_ = active_request_id_;
    bool should_notify = notify_when_snapshot_ready_ && result_ready_ &&
                         result_request_id_ == pending_prompt_request_id_;
    if (should_notify) {
        notify_when_snapshot_ready_ = false;
    }
    lock.unlock();
    if (should_notify && g_prompt_refresh_allowed.load(std::memory_order_acquire)) {
        (void)ic_push_key_event(IC_KEY_EVENT_PROMPT_REFRESH);
    }
}

std::optional<std::string> AsyncGitPromptManager::consume_ready_prompt() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!async_enabled_cached_ || !refresh_pending_ || !result_ready_ || !prompt_snapshot_ready_ ||
        pending_prompt_request_id_ != result_request_id_ || placeholder_offsets_.empty()) {
        return std::nullopt;
    }
    std::string updated = pending_prompt_;
    size_t inserted = 0;
    for (size_t offset : placeholder_offsets_) {
        (void)updated.insert(offset + inserted, pending_segment_);
        inserted += pending_segment_.size();
    }
    reset_async_state_locked();
    lock.unlock();
    return updated;
}

void AsyncGitPromptManager::start_worker_locked(size_t request_id,
                                                const GitRepositoryContext& context) {
    worker_running_ = true;
    worker_request_id_ = request_id;
    std::thread worker([this, request_id, context] {
        bool dirty = git_has_changes(context.workdir);
        GitStatusSnapshot snapshot = capture_git_status_snapshot(context);
        git_prompt_cache().store_status_result(context, dirty, snapshot,
                                               std::chrono::steady_clock::now());
        std::string segment = format_git_status_segment(dirty);
        handle_worker_result(request_id, std::move(segment));
    });
    worker.detach();
}

void AsyncGitPromptManager::handle_worker_result(size_t request_id, std::string segment) {
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        if (!async_enabled_cached_ || request_id != active_request_id_) {
            return;
        }
        pending_segment_ = std::move(segment);
        result_ready_ = true;
        result_request_id_ = request_id;
        if (refresh_pending_) {
            if (prompt_snapshot_ready_) {
                should_notify = true;
            } else {
                notify_when_snapshot_ready_ = true;
            }
        }
    }
    cv_.notify_all();
    if (should_notify && g_prompt_refresh_allowed.load(std::memory_order_acquire)) {
        (void)ic_push_key_event(IC_KEY_EVENT_PROMPT_REFRESH);
    }
}

void AsyncGitPromptManager::reset_async_state_locked() {
    refresh_pending_ = false;
    awaiting_async_result_ = false;
    prompt_snapshot_ready_ = false;
    notify_when_snapshot_ready_ = false;
    result_ready_ = false;
    placeholder_offsets_.clear();
    pending_prompt_.clear();
    pending_segment_.clear();
}

AsyncGitPromptManager& git_prompt_async_manager() {
    static AsyncGitPromptManager manager;
    return manager;
}

void append_git_segment(std::string& builder, PromptContext context) {
    if (config::minimal_mode || config::secure_mode) {
        return;
    }
    if (context == PromptContext::Primary) {
        git_prompt_async_manager().append_segment(builder);
    } else {
        builder += render_git_segment_now();
    }
}

int exit_status_value() {
    std::string status = get_exit_status();
    return numeric_utils::parse_exit_status_or(status, 0, false);
}

std::string status_symbol() {
    static const char* arrow = u8"\u279C";
    int status = exit_status_value();
    if (status == 0) {
        return std::string("[b color=#2dd881]") + arrow + "[/]";
    }

    int signal_status = status - 128;
    if (signal_status == SIGTSTP || signal_status == SIGSTOP || signal_status == SIGTTIN ||
        signal_status == SIGTTOU) {
        return std::string("[b color=#ffd166]") + arrow + "[/]";
    }

    return std::string("[b color=#ff6b6b]") + arrow + "[/]";
}

std::string get_version_short() {
    std::string version = get_version();
    auto pos = version.find(' ');
    if (pos != std::string::npos) {
        version = version.substr(0, pos);
    }
    pos = version.find_last_of('.');
    if (pos != std::string::npos) {
        return version.substr(0, pos);
    }
    return version;
}

std::string get_version_full() {
    return get_version();
}

std::string get_exit_status() {
    return get_env("?");
}

std::string get_job_count() {
    size_t count = JobManager::instance().get_all_jobs().size();
    return std::to_string(count);
}

char prompt_dollar() {
    if (geteuid() == 0) {
        return '#';
    }
    return '$';
}

char prompt_eol_default_symbol() {
    if (geteuid() == 0) {
        return '#';
    }
    return '%';
}

char to_ascii(std::uint8_t value) {
    return static_cast<char>(value);
}

std::string expand_prompt_string(const std::string& templ, PromptContext context) {
    std::string result;
    result.reserve(templ.size() + 16);

    for (size_t i = 0; i < templ.size(); ++i) {
        char ch = templ[i];
        if (ch != '\\') {
            result.push_back(ch);
            continue;
        }

        // Preserve the String Terminator used by raw OSC sequences (ESC + '\\').
        // External prompt renderers such as Oh My Posh emit OSC 8 hyperlinks in
        // this form; interpreting the backslash as a PS1 escape corrupts them.
        if (i > 0 && templ[i - 1] == '\033') {
            result.push_back(ch);
            continue;
        }

        ++i;
        if (i >= templ.size()) {
            result.push_back('\\');
            break;
        }

        char code = templ[i];
        switch (code) {
            case 'a':
                result.push_back('\a');
                break;
            case 'd':
                result += format_time("%a %b %e");
                break;
            case 'D': {
                if (i + 1 < templ.size() && templ[i + 1] == '{') {
                    size_t closing = templ.find('}', i + 2);
                    if (closing != std::string::npos) {
                        std::string fmt = templ.substr(i + 2, closing - (i + 2));
                        result += format_time(fmt.c_str());
                        i = closing;
                        break;
                    }
                }
                result.push_back('D');
                break;
            }
            case 'e':
            case 'E':
                result.push_back('\033');
                break;
            case 'h':
                result += get_hostname(false);
                break;
            case 'H':
                result += get_hostname(true);
                break;
            case 'j':
                result += get_job_count();
                break;
            case 'l':
                result += get_terminal_name();
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 's':
                result += get_shell_name();
                break;
            case 't':
                result += format_time("%H:%M:%S");
                break;
            case 'T':
                result += format_time("%I:%M:%S");
                break;
            case '@':
                result += format_time("%I:%M %p");
                break;
            case 'A':
                result += format_time("%H:%M");
                break;
            case 'u':
                result += get_username();
                break;
            case 'v':
                result += get_version_short();
                break;
            case 'V':
                result += get_version_full();
                break;
            case 'w':
                result += get_cwd(true, false);
                break;
            case 'W':
                result += get_cwd(true, true);
                break;
            case 'p':
                result += get_abbreviated_cwd();
                break;
            case 'S':
                result += status_symbol();
                break;
            case 'g':
                append_git_segment(result, context);
                break;
            case '$':
                result.push_back(prompt_dollar());
                break;
            case '?':
                result += get_exit_status();
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '[':
            case ']':
                break;
            default: {
                if (code >= '0' && code <= '7') {
                    int value = code - '0';
                    int digits = 1;
                    while (digits < 3 && (i + 1) < templ.size()) {
                        char next = templ[i + 1];
                        if (next < '0' || next > '7') {
                            break;
                        }
                        value = (value * 8) + (next - '0');
                        ++i;
                        ++digits;
                    }
                    result.push_back(to_ascii(static_cast<std::uint8_t>(value & 0xFF)));
                } else {
                    result.push_back(code);
                }
                break;
            }
        }
    }

    return result;
}

std::string get_ps(const char* name, const std::string& fallback) {
    if (cjsh_env::shell_variable_is_set(name)) {
        return cjsh_env::get_shell_variable_value(name);
    }
    return fallback;
}

}  // namespace

std::string default_primary_prompt_template() {
    if (config::minimal_mode || config::secure_mode) {
        return "cjsh> ";
    }
    return "\\S  [color=#5fd7ff]\\p[/color] \\g";
}

std::string default_secondary_prompt_template() {
    return "[ic-hint]> [/ic-hint]";
}

std::string default_history_search_prompt_template() {
    return "history search: ";
}

std::string default_command_palette_prompt_template() {
    return "command palette: ";
}

std::string render_primary_prompt() {
    std::string ps1 = config::prompt_vars_enabled ? get_ps("PS1", default_primary_prompt_template())
                                                  : std::string("cjsh> ");
    git_prompt_async_manager().begin_render();
    std::string prompt_text = expand_prompt_string(ps1, PromptContext::Primary);
    git_prompt_async_manager().finalize_render(prompt_text);
    return prompt_text;
}

std::string render_prompt_eol_mark() {
    std::string eol_mark;
    if (cjsh_env::shell_variable_is_set("PROMPT_EOL_MARK")) {
        eol_mark = cjsh_env::get_shell_variable_value("PROMPT_EOL_MARK");
    } else {
        eol_mark = "[b reverse]";
        eol_mark.push_back(prompt_eol_default_symbol());
        eol_mark += "[/]";
    }
    return expand_prompt_string(eol_mark, PromptContext::Primary);
}

std::string render_trace_prompt() {
    std::string ps4 = get_ps("PS4", "+ ");
    std::string trace_prompt = expand_prompt_string(ps4, PromptContext::Primary);
    if (g_shell != nullptr) {
        if (Parser* parser = g_shell->get_parser()) {
            parser->expand_env_vars(trace_prompt);
        }
    }
    return trace_prompt;
}

std::string render_right_prompt() {
    if (cjsh_env::shell_variable_is_set("RPROMPT")) {
        std::string rprompt = cjsh_env::get_shell_variable_value("RPROMPT");
        if (rprompt.empty()) {
            return {};
        }
        return expand_prompt_string(rprompt, PromptContext::Right);
    }

    if (!cjsh_env::shell_variable_is_set("RPS1")) {
        return {};
    }

    std::string rps1 = cjsh_env::get_shell_variable_value("RPS1");
    if (rps1.empty()) {
        return {};
    }
    return expand_prompt_string(rps1, PromptContext::Right);
}

std::string render_secondary_prompt() {
    if (!config::prompt_vars_enabled) {
        return "> ";
    }
    if (!cjsh_env::shell_variable_is_set("PS2")) {
        return {};
    }
    std::string ps2 = cjsh_env::get_shell_variable_value("PS2");
    if (ps2.empty()) {
        return {};
    }
    return expand_prompt_string(ps2, PromptContext::Secondary);
}

std::string render_history_search_prompt() {
    std::string ps5 = config::prompt_vars_enabled
                          ? get_ps("PS5", default_history_search_prompt_template())
                          : default_history_search_prompt_template();
    return expand_prompt_string(ps5, PromptContext::Secondary);
}

std::string render_command_palette_prompt() {
    std::string ps6 = config::prompt_vars_enabled
                          ? get_ps("PS6", default_command_palette_prompt_template())
                          : default_command_palette_prompt_template();
    return expand_prompt_string(ps6, PromptContext::Secondary);
}

bool apply_transient_final_prompt_if_configured() {
    if (!config::prompt_vars_enabled) {
        return false;
    }

    const bool has_ps1_final = cjsh_env::shell_variable_is_set("PS1_FINAL");
    const bool has_rps1_final = cjsh_env::shell_variable_is_set("RPS1_FINAL");
    if (!has_ps1_final && !has_rps1_final) {
        return false;
    }

    const char* final_prompt_ptr = nullptr;
    std::string final_prompt_text;
    if (has_ps1_final) {
        std::string ps1_final = cjsh_env::get_shell_variable_value("PS1_FINAL");
        final_prompt_text = expand_prompt_string(ps1_final, PromptContext::Final);
        final_prompt_ptr = final_prompt_text.c_str();
    }

    const char* final_right_prompt_ptr = nullptr;
    if (has_rps1_final) {
        std::string rps1_final = cjsh_env::get_shell_variable_value("RPS1_FINAL");
        g_transient_final_right_prompt = expand_prompt_string(rps1_final, PromptContext::Right);
        final_right_prompt_ptr = g_transient_final_right_prompt.c_str();
    }

    return ic_current_loop_reset(nullptr, final_prompt_ptr, final_right_prompt_ptr);
}

bool advance_with_transient_final_prompt(const std::string& new_buffer) {
    const char* final_prompt_ptr = nullptr;
    const char* final_right_prompt_ptr = nullptr;
    std::string final_prompt_text;
    std::string final_right_prompt_text;

    if (config::prompt_vars_enabled) {
        if (cjsh_env::shell_variable_is_set("PS1_FINAL")) {
            final_prompt_text = expand_prompt_string(
                cjsh_env::get_shell_variable_value("PS1_FINAL"), PromptContext::Final);
            final_prompt_ptr = final_prompt_text.c_str();
        }
        if (cjsh_env::shell_variable_is_set("RPS1_FINAL")) {
            final_right_prompt_text = expand_prompt_string(
                cjsh_env::get_shell_variable_value("RPS1_FINAL"), PromptContext::Right);
            final_right_prompt_ptr = final_right_prompt_text.c_str();
        }
    }

    return ic_current_loop_advance_with_prompt(new_buffer.c_str(), final_prompt_ptr,
                                               final_right_prompt_ptr);
}

void execute_prompt_command() {
    if (!g_shell) {
        return;
    }
    if (config::secure_mode || config::is_posix_mode()) {
        return;
    }
    std::string command = get_env("PROMPT_COMMAND");
    if (command.empty()) {
        return;
    }
    (void)g_shell->execute(command);
}

void initialize_colors() {
    if (!config::colors_enabled) {
        (void)ic_enable_color(false);
        return;
    }

    if (!terminal_supports_color()) {
        config::colors_enabled = false;
        (void)ic_enable_color(false);
        return;
    }

    (void)ic_enable_color(true);

    if (!config::syntax_highlighting_enabled) {
        return;
    }

    for (const auto& pair : token_constants::default_styles()) {
        std::string style_name = pair.first;
        if (style_name.rfind("ic-", 0) != 0) {
            style_name = "cjsh-";
            style_name += pair.first;
        }
        ic_style_def(style_name.c_str(), pair.second.c_str());
    }
}

void apply_terminal_window_title() {
    if (!config::interactive_mode || config::is_posix_mode()) {
        return;
    }
    FILE* output = isatty(STDOUT_FILENO) ? stdout : (isatty(STDERR_FILENO) ? stderr : nullptr);
    if (output == nullptr) {
        return;
    }

    std::string title;
    std::string twinprompt = cjsh_env::get_shell_variable_value("TWINPROMPT");
    if (!twinprompt.empty()) {
        (void)title.assign(twinprompt);
    } else {
        title = build_default_terminal_title();
        if (!title.empty()) {
            (void)cjsh_env::set_shell_variable_value("TWINPROMPT", title);
        }
    }

    if (title.empty()) {
        return;
    }

    (void)std::fprintf(output, "\033]0;%s\007", title.c_str());
    (void)std::fflush(output);
}

bool handle_async_prompt_refresh() {
    if (!g_prompt_refresh_allowed.load(std::memory_order_acquire)) {
        return false;
    }
    auto updated_prompt = git_prompt_async_manager().consume_ready_prompt();
    if (!updated_prompt) {
        return false;
    }
    const char* buffer = ic_get_buffer();
    if (buffer == nullptr || buffer[0] != '\0') {
        return false;
    }
    size_t cursor_pos = 0;
    if (!ic_get_cursor_pos(&cursor_pos) || cursor_pos != 0) {
        return false;
    }
    return ic_current_loop_reset(nullptr, updated_prompt->c_str(), nullptr);
}

void set_prompt_refresh_allowed(bool allowed) {
    g_prompt_refresh_allowed.store(allowed, std::memory_order_release);
}
}  // namespace prompt
