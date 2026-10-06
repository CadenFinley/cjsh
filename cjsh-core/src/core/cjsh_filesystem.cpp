/*
  cjsh_filesystem.cpp

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

#include "cjsh_filesystem.h"
#include "file_snapshot.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "error_out.h"
#include "parser.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace cjsh_filesystem {

namespace {
enum class CacheUsage : std::uint8_t;
std::string resolve_command_with_cache(const std::string& name, CacheUsage usage);
}  // namespace

const std::filesystem::path& g_user_home_path() {
    static const std::filesystem::path path = [] {
        std::string home = cjsh_env::get_shell_variable_value("HOME");
        if (home.empty()) {
            print_error({ErrorType::UNKNOWN_ERROR,
                         ErrorSeverity::WARNING,
                         "filesystem",
                         "HOME environment variable not set or empty. Using /tmp as fallback.",
                         {}});
            return std::filesystem::path("/tmp");
        }
        return std::filesystem::path(home);
    }();
    return path;
}

namespace {
std::filesystem::path expand_leading_tilde_path(std::string_view raw_value) {
    std::filesystem::path candidate;

    if (raw_value == "~") {
        candidate = g_user_home_path();
    } else if (raw_value.rfind("~/", 0) == 0) {
        candidate = g_user_home_path();
        if (raw_value.size() > 2) {
            candidate /= std::string(raw_value.substr(2));
        }
    } else {
        candidate = std::filesystem::path(raw_value);
    }

    return candidate;
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool path_is_directory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool path_is_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

void close_fd_if_valid(int fd) {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

class ScopedFd {
   public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {
    }

    ~ScopedFd() {
        close_fd_if_valid(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const {
        return fd_;
    }

    int release() {
        int released = fd_;
        fd_ = -1;
        return released;
    }

    void reset(int fd = -1) {
        close_fd_if_valid(fd_);
        fd_ = fd;
    }

   private:
    int fd_;
};
}  // namespace

std::filesystem::path normalize_override_path(std::string_view raw_value) {
    if (raw_value.empty()) {
        return {};
    }

    std::filesystem::path candidate = expand_leading_tilde_path(raw_value);

    std::error_code abs_ec;
    if (!candidate.is_absolute()) {
        auto absolute_candidate = std::filesystem::absolute(candidate, abs_ec);
        if (!abs_ec) {
            candidate = std::move(absolute_candidate);
        }
    }

    return candidate.lexically_normal();
}

const std::filesystem::path& g_cjsh_config_path() {
    static const std::filesystem::path path = config::config_directory.empty()
                                                  ? g_user_home_path() / ".config" / "cjsh"
                                                  : std::filesystem::path(config::config_directory);
    return path;
}

const std::filesystem::path& g_cjsh_cache_path() {
    static const std::filesystem::path path = g_user_home_path() / ".cache" / "cjsh";
    return path;
}

const std::filesystem::path& g_cjsh_profile_path() {
    static const std::filesystem::path path =
        (config::config_directory.empty() ? g_user_home_path() : g_cjsh_config_path()) /
        ".cjprofile";
    return path;
}

const std::filesystem::path& g_cjsh_env_path() {
    static const std::filesystem::path path =
        (config::config_directory.empty() ? g_user_home_path() : g_cjsh_config_path()) / ".cjshenv";
    return path;
}

const std::filesystem::path& g_cjsh_source_path() {
    static const std::filesystem::path path =
        (config::config_directory.empty() ? g_user_home_path() : g_cjsh_config_path()) / ".cjshrc";
    return path;
}

const std::filesystem::path& g_cjsh_logout_path() {
    static const std::filesystem::path path =
        (config::config_directory.empty() ? g_user_home_path() : g_cjsh_config_path()) /
        ".cjlogout";
    return path;
}

const std::filesystem::path& g_cjsh_profile_alt_path() {
    static const std::filesystem::path path = g_cjsh_config_path() / ".cjprofile";
    return path;
}

const std::filesystem::path& g_cjsh_env_alt_path() {
    static const std::filesystem::path path = g_cjsh_config_path() / ".cjshenv";
    return path;
}

const std::filesystem::path& g_cjsh_source_alt_path() {
    static const std::filesystem::path path = g_cjsh_config_path() / ".cjshrc";
    return path;
}

const std::filesystem::path& g_cjsh_logout_alt_path() {
    static const std::filesystem::path path = g_cjsh_config_path() / ".cjlogout";
    return path;
}

namespace {
struct HistoryPathState {
    std::filesystem::path path;
    std::optional<std::string> override_value;
    bool finalized = false;
};

HistoryPathState& history_path_state() {
    static HistoryPathState state;
    return state;
}
}  // namespace

const std::filesystem::path& g_cjsh_history_path() {
    auto& state = history_path_state();
    if (!state.override_value || !state.finalized) {
        std::string custom_history = cjsh_env::get_shell_variable_value("CJSH_HISTORY_FILE");
        if (!state.override_value || *state.override_value != custom_history) {
            state.path = custom_history.empty() ? g_cjsh_cache_path() / "history.txt"
                                                : normalize_override_path(custom_history);
            state.override_value = std::move(custom_history);
        }
    }
    return state.path;
}

void finalize_history_path() {
    auto& state = history_path_state();
    // Refresh an early startup lookup, but leave scripts that never used history lazy.
    if (state.override_value) {
        (void)g_cjsh_history_path();
    }
    state.finalized = true;
}

const std::filesystem::path& g_cjsh_first_boot_path() {
    static const std::filesystem::path path = g_cjsh_cache_path() / ".first_boot";
    return path;
}

const std::filesystem::path& g_cjsh_generated_completions_path() {
    static const std::filesystem::path path = g_cjsh_cache_path() / "generated_completions";
    return path;
}

namespace {
std::string describe_errno(int err) {
    return std::system_category().message(err);
}

constexpr const char* kDefaultCjshArgv0 = "cjsh";

std::string strip_login_prefix(std::string token) {
    if (!token.empty() && token.front() == '-') {
        (void)token.erase(token.begin());
    }
    return token;
}

std::filesystem::path normalize_path(const std::filesystem::path& raw_path) {
    if (raw_path.empty()) {
        return raw_path;
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(raw_path, ec);
    if (!ec && !canonical.empty()) {
        return canonical;
    }

    ec.clear();
    std::filesystem::path weakly_canonical_path = std::filesystem::weakly_canonical(raw_path, ec);
    if (!ec && !weakly_canonical_path.empty()) {
        return weakly_canonical_path;
    }

    return raw_path.lexically_normal();
}

std::optional<std::filesystem::path> resolve_running_executable_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return std::nullopt;
    }

    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0 || buffer.empty()) {
        return std::nullopt;
    }

    return std::filesystem::path(buffer.data());
#elif defined(__linux__)
    std::array<char, 4096> buffer{};
    ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (bytes_read <= 0) {
        return std::nullopt;
    }

    buffer[static_cast<size_t>(bytes_read)] = '\0';
    return std::filesystem::path(buffer.data());
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> resolve_executable_token(const std::string& token) {
    std::string cleaned = strip_login_prefix(token);
    if (cleaned.empty()) {
        return std::nullopt;
    }

    std::filesystem::path candidate(cleaned);
    if (!candidate.is_absolute()) {
        if (cleaned.find('/') != std::string::npos) {
            std::error_code ec;
            std::filesystem::path absolute_candidate = std::filesystem::absolute(candidate, ec);
            if (!ec) {
                candidate = std::move(absolute_candidate);
            }
        } else {
            std::string resolved = find_executable_in_path(cleaned);
            if (!resolved.empty()) {
                candidate = std::filesystem::path(resolved);
            }
        }
    }

    if (!path_exists(candidate)) {
        return std::nullopt;
    }

    if (path_is_directory(candidate)) {
        return std::nullopt;
    }

    return normalize_path(candidate);
}

bool path_is_executable(const std::filesystem::path& candidate) {
    std::error_code ec;
    const auto status = std::filesystem::status(candidate, ec);
    return !ec && std::filesystem::exists(status) && !std::filesystem::is_directory(status) &&
           ::access(candidate.c_str(), X_OK) == 0;
}

}  // namespace

namespace {
template <typename Callback>
bool for_each_path_segment(std::string_view path_str, Callback&& callback) {
    size_t start = 0;
    while (start < path_str.size()) {
        size_t pos = path_str.find(':', start);
        size_t end = (pos != std::string::npos) ? pos : path_str.size();
        if (callback(path_str.substr(start, end - start))) {
            return true;
        }
        start = (pos != std::string::npos) ? pos + 1 : path_str.size();
    }
    return false;
}
}  // namespace

namespace {

enum class CacheUsage : std::uint8_t {
    Query,
    Execution,
    Manual
};

thread_local size_t interactive_path_lookup_depth = 0;

struct CachedExecutable {
    std::string path;
    std::uint64_t hits{0};
    std::time_t last_used{0};
    bool manually_added{false};
};

std::string current_path_env_value() {
    return cjsh_env::get_shell_variable_value("PATH");
}

bool is_cacheable_command(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos;
}

std::string resolve_explicit_command(const std::string& name) {
    if (name.empty()) {
        return {};
    }

    std::filesystem::path candidate(name);
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::path(safe_current_directory()) / candidate;
    }
    candidate = candidate.lexically_normal();

    return path_is_executable(candidate) ? candidate.string() : std::string{};
}

std::string scan_path_for_command(const std::string& name, std::string_view path_value) {
    std::string resolved;
    (void)for_each_path_segment(path_value, [&](std::string_view raw_segment) {
        if (raw_segment.empty()) {
            return false;
        }
        std::filesystem::path directory_path(raw_segment);
        std::filesystem::path candidate = directory_path / name;
        if (path_is_executable(candidate)) {
            resolved = candidate.string();
            return true;
        }
        return false;
    });
    return resolved;
}

class PathHashCache {
   public:
    std::string resolve(const std::string& name, CacheUsage usage) {
        std::string current_path = current_path_env_value();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_snapshot_locked(current_path);

        const bool interactive = usage == CacheUsage::Query && interactive_path_lookup_depth > 0;
        if (interactive) {
            auto cached = interactive_results_.find(name);
            if (cached != interactive_results_.end()) {
                return cached->second;
            }
        }

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        auto it = entries_.find(name);
        if (it != entries_.end()) {
            if (entry_is_valid(it->second)) {
                if (usage == CacheUsage::Execution) {
                    it->second.hits++;
                }
                if (usage == CacheUsage::Manual) {
                    it->second.manually_added = true;
                }
                it->second.last_used = now;
                if (interactive) {
                    interactive_results_[name] = it->second.path;
                }
                return it->second.path;
            }
            (void)entries_.erase(it);
        }

        if (current_path.empty()) {
            return {};
        }

        std::string resolved;
        if (interactive) {
            index_interactive_names_locked(current_path);
            if (!interactive_index_complete_) {
                // A searchable directory need not be readable. Preserve lookup
                // semantics when its names cannot be enumerated.
                resolved = scan_path_for_command(name, current_path);
            } else {
                auto candidates = interactive_paths_.find(name);
                if (candidates == interactive_paths_.end()) {
                    return {};
                }
                for (const auto& candidate : candidates->second) {
                    if (path_is_executable(candidate)) {
                        resolved = candidate;
                        break;
                    }
                }
            }
        } else {
            resolved = scan_path_for_command(name, current_path);
        }
        if (!resolved.empty()) {
            CachedExecutable entry;
            entry.path = resolved;
            entry.hits = (usage == CacheUsage::Execution) ? 1 : 0;
            entry.manually_added = (usage == CacheUsage::Manual);
            entry.last_used = now;
            entries_[name] = std::move(entry);
        }

        if (interactive) {
            interactive_results_[name] = resolved;
        }

        return resolved;
    }

    std::vector<std::string> executables_in_path() {
        std::string current_path = current_path_env_value();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_snapshot_locked(current_path);
        seed_locked(current_path);

        std::vector<std::string> executables;
        executables.reserve(entries_.size());
        for (const auto& [command, entry] : entries_) {
            (void)entry;
            executables.push_back(command);
        }

        return executables;
    }

    std::vector<PathHashEntry> entries() {
        std::string current_path = current_path_env_value();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_snapshot_locked(current_path);

        std::vector<PathHashEntry> snapshot;
        snapshot.reserve(entries_.size());
        for (const auto& [command, entry] : entries_) {
            snapshot.push_back(
                {command, entry.path, entry.hits, entry.last_used, entry.manually_added});
        }

        std::sort(snapshot.begin(), snapshot.end(),
                  [](const PathHashEntry& lhs, const PathHashEntry& rhs) {
                      if (lhs.hits == rhs.hits) {
                          return lhs.command < rhs.command;
                      }
                      return lhs.hits > rhs.hits;
                  });

        return snapshot;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_snapshot_locked(current_path_env_value());
        entries_.clear();
        seeded_ = false;
        reset_interactive_locked();
    }

    void reset_interactive() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!interactive_names_ready_) {
            reset_interactive_locked();
            return;
        }
        ensure_snapshot_locked(current_path_env_value());
        // Always recheck emitted candidates' permissions and symlink targets.
        // Reuse the name index when directory metadata is unchanged and old
        // enough to distinguish edits on filesystems with coarse timestamps.
        interactive_results_.clear();
        if (std::any_of(interactive_directories_.begin(), interactive_directories_.end(),
                        [](const auto& entry) {
                            return !entry.second.can_reuse_cached_data(
                                FileSnapshot::read(entry.first));
                        })) {
            reset_interactive_locked();
        }
    }

    std::vector<std::string> completion_candidates() {
        const std::string current_path = current_path_env_value();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_snapshot_locked(current_path);
        index_interactive_names_locked(current_path);
        std::vector<std::string> names;
        names.reserve(interactive_paths_.size());
        for (const auto& [name, paths] : interactive_paths_) {
            (void)paths;
            names.push_back(name);
        }
        // Retain known commands in searchable directories that cannot be listed.
        for (const auto& [name, entry] : entries_) {
            (void)entry;
            if (interactive_paths_.count(name) == 0) {
                names.push_back(name);
            }
        }
        return names;
    }

   private:
    void reset_interactive_locked() {
        interactive_results_.clear();
        interactive_paths_.clear();
        interactive_directories_.clear();
        interactive_names_ready_ = false;
        interactive_index_complete_ = true;
    }

    void index_interactive_names_locked(const std::string& path_value) {
        if (!interactive_names_ready_) {
            // Incomplete words usually do not exist anywhere in PATH. Read names
            // once, without stat/access on every file (especially costly on WSL
            // mounts), then reject all those prefixes entirely in memory.
            std::unordered_set<std::string> visited;
            (void)for_each_path_segment(path_value, [&](std::string_view raw_segment) {
                if (raw_segment.empty() || !visited.emplace(raw_segment).second) {
                    return false;
                }
                interactive_directories_.emplace_back(
                    std::string(raw_segment),
                    FileSnapshot::read(std::filesystem::path(raw_segment)));
                std::error_code ec;
                std::filesystem::directory_iterator it(std::filesystem::path(raw_segment), ec);
                for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                    interactive_paths_[it->path().filename().string()].push_back(
                        it->path().string());
                }
                if (ec && ec != std::errc::no_such_file_or_directory &&
                    ec != std::errc::not_a_directory) {
                    interactive_index_complete_ = false;
                }
                return false;
            });
            interactive_names_ready_ = true;
        }
    }

    static bool entry_is_valid(const CachedExecutable& entry) {
        return path_is_executable(entry.path);
    }

    void ensure_snapshot_locked(const std::string& current_path) {
        if (current_path != path_snapshot_) {
            relative_path_ = false;
            (void)for_each_path_segment(current_path, [&](std::string_view segment) {
                relative_path_ = !segment.empty() && segment.front() != '/';
                return relative_path_;
            });
        }
        const std::string cwd = relative_path_ ? safe_current_directory() : std::string{};
        if (current_path != path_snapshot_ || cwd != cwd_snapshot_) {
            entries_.clear();
            path_snapshot_ = current_path;
            cwd_snapshot_ = cwd;
            seeded_ = false;
            reset_interactive_locked();
        }
    }

    void seed_locked(const std::string& path_value) {
        if (seeded_ || path_value.empty()) {
            return;
        }

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        (void)for_each_path_segment(path_value, [&](std::string_view raw_segment) {
            if (raw_segment.empty()) {
                return false;
            }

            std::filesystem::path directory_path(raw_segment);
            if (!path_is_directory(directory_path)) {
                return false;
            }

            std::error_code ec;
            std::filesystem::directory_iterator it(
                directory_path, std::filesystem::directory_options::skip_permission_denied, ec);
            if (ec) {
                return false;
            }

            for (; it != std::filesystem::directory_iterator(); (void)it.increment(ec)) {
                if (ec) {
                    break;
                }

                const auto& entry = *it;
                auto status = entry.status(ec);
                if (ec) {
                    ec.clear();
                    continue;
                }

                if (!std::filesystem::is_regular_file(status)) {
                    continue;
                }

                auto perms = status.permissions();
                constexpr auto exec_mask = std::filesystem::perms::owner_exec |
                                           std::filesystem::perms::group_exec |
                                           std::filesystem::perms::others_exec;
                if ((perms & exec_mask) == std::filesystem::perms::none) {
                    continue;
                }

                std::string command = entry.path().filename().string();
                if (entries_.find(command) != entries_.end()) {
                    continue;
                }

                (void)entries_.emplace(std::move(command),
                                       CachedExecutable{entry.path().string(), 0, now, false});
            }

            return false;
        });

        seeded_ = true;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, CachedExecutable> entries_;
    std::string path_snapshot_;
    std::string cwd_snapshot_;
    bool relative_path_{false};
    bool seeded_{false};
    std::unordered_map<std::string, std::string> interactive_results_;
    std::unordered_map<std::string, std::vector<std::string>> interactive_paths_;
    std::vector<std::pair<std::string, FileSnapshot>> interactive_directories_;
    bool interactive_names_ready_{false};
    bool interactive_index_complete_{true};
};

PathHashCache g_path_hash_cache;

PathHashCache& path_hash_cache() {
    return g_path_hash_cache;
}

std::string resolve_command_with_cache(const std::string& name, CacheUsage usage) {
    if (!is_cacheable_command(name)) {
        return resolve_explicit_command(name);
    }

    return path_hash_cache().resolve(name, usage);
}

std::vector<std::string> get_executables_from_path_cache() {
    return path_hash_cache().executables_in_path();
}

std::vector<PathHashEntry> get_entries_from_path_cache() {
    return path_hash_cache().entries();
}

void reset_path_cache_entries() {
    path_hash_cache().reset();
}

}  // namespace

ScopedInteractivePathLookup::ScopedInteractivePathLookup() {
    ++interactive_path_lookup_depth;
}

ScopedInteractivePathLookup::~ScopedInteractivePathLookup() {
    --interactive_path_lookup_depth;
}

void reset_interactive_path_cache() {
    path_hash_cache().reset_interactive();
}

std::vector<std::string> get_path_completion_candidates() {
    return path_hash_cache().completion_candidates();
}

std::string safe_current_directory() {
    char* cwd = ::getcwd(nullptr, 0);
    if (cwd != nullptr) {
        std::string result(cwd);
        std::free(cwd);
        return result;
    }

    if (cjsh_env::shell_variable_is_set("PWD")) {
        std::string env_pwd = cjsh_env::get_shell_variable_value("PWD");
        if (!env_pwd.empty()) {
            return env_pwd;
        }
    }

    const auto& home_path = g_user_home_path();
    if (!home_path.empty()) {
        return home_path.string();
    }

    return "/";
}

std::string abbreviate_home_path(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    const std::string home = g_user_home_path().string();
    if (home.empty()) {
        return path;
    }

    if (path == home) {
        return "~";
    }

    if (path.rfind(home + "/", 0) == 0) {
        return "~" + path.substr(home.size());
    }

    return path;
}

std::string format_path_for_display(const std::string& path, bool abbreviate_home,
                                    bool basename_only) {
    std::string display = abbreviate_home ? abbreviate_home_path(path) : path;
    if (!basename_only) {
        return display;
    }

    if (display == "~") {
        return display;
    }

    std::filesystem::path path_obj(display);
    std::string base = path_obj.filename().string();
    if (base.empty()) {
        base = path_obj.root_path().string();
    }
    return base;
}

std::string formatted_current_directory(bool abbreviate_home, bool basename_only) {
    return format_path_for_display(safe_current_directory(), abbreviate_home, basename_only);
}

Result<int> safe_open(const std::string& path, int flags, mode_t mode) {
    int fd = ::open(path.c_str(), flags, mode);
    if (fd == -1) {
        return Result<int>::error("Failed to open file '" + path + "': " + describe_errno(errno));
    }
    return Result<int>::ok(fd);
}

Result<void> safe_dup2(int oldfd, int newfd) {
    if (::dup2(oldfd, newfd) == -1) {
        return Result<void>::error("Failed to duplicate file descriptor " + std::to_string(oldfd) +
                                   " to " + std::to_string(newfd) + ": " + describe_errno(errno));
    }
    return Result<void>::ok();
}

void safe_close(int fd) {
    close_fd_if_valid(fd);
}

Result<void> redirect_fd(const std::string& file, int target_fd, int flags, bool force_overwrite) {
    const bool noclobber = !force_overwrite && (flags & O_TRUNC) && g_shell &&
                           g_shell->get_shell_option(ShellOption::Noclobber);
    auto open_result = safe_open(file, noclobber ? flags & ~(O_TRUNC | O_CREAT) : flags, 0666);
    if (noclobber && open_result.is_error() && errno == ENOENT) {
        open_result = safe_open(file, (flags & ~O_TRUNC) | O_CREAT | O_EXCL, 0666);
    } else if (noclobber && open_result.is_ok()) {
        struct stat opened{};
        if (fstat(open_result.value(), &opened) == -1 || S_ISREG(opened.st_mode)) {
            safe_close(open_result.value());
            errno = EEXIST;
            return Result<void>::error("cannot overwrite existing file (noclobber is set)");
        }
    }
    if (open_result.is_error()) {
        return Result<void>::error(open_result.error());
    }

    ScopedFd file_fd(open_result.value());
    if (file_fd.get() == target_fd) {
        (void)file_fd.release();
        return Result<void>::ok();
    }

    auto dup_result = safe_dup2(file_fd.get(), target_fd);
    if (dup_result.is_error()) {
        return dup_result;
    }

    return Result<void>::ok();
}

Result<void> set_close_on_exec(int fd) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1) {
        return Result<void>::error("failed to get file descriptor flags for fd " +
                                   std::to_string(fd) + ": " + describe_errno(errno));
    }

    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return Result<void>::error("failed to set close-on-exec on fd " + std::to_string(fd) +
                                   ": " + describe_errno(errno));
    }

    return Result<void>::ok();
}

Result<void> create_pipe_cloexec(int pipe_fds[2]) {
    if (::pipe(pipe_fds) == -1) {
        return Result<void>::error("failed to create pipe: " + describe_errno(errno));
    }

    ScopedFd read_end(pipe_fds[0]);
    ScopedFd write_end(pipe_fds[1]);

    auto read_result = set_close_on_exec(read_end.get());
    if (read_result.is_error()) {
        return Result<void>::error("failed to secure pipe read end: " + read_result.error());
    }

    auto write_result = set_close_on_exec(write_end.get());
    if (write_result.is_error()) {
        return Result<void>::error("failed to secure pipe write end: " + write_result.error());
    }

    pipe_fds[0] = read_end.release();
    pipe_fds[1] = write_end.release();

    return Result<void>::ok();
}

Result<void> duplicate_pipe_read_end_to_fd(int (&pipe_fds)[2], int target_fd) {
    safe_close(pipe_fds[1]);

    if (pipe_fds[0] == target_fd) {
        pipe_fds[1] = -1;
        pipe_fds[0] = -1;
        return Result<void>::ok();
    }

    auto dup_result = safe_dup2(pipe_fds[0], target_fd);
    safe_close(pipe_fds[0]);
    pipe_fds[1] = -1;
    pipe_fds[0] = -1;
    if (dup_result.is_error()) {
        return Result<void>::error(dup_result.error());
    }

    return Result<void>::ok();
}

void close_pipe(int pipe_fds[2]) {
    safe_close(pipe_fds[0]);
    safe_close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
}

namespace {
Result<void> write_content_with_permissions(const std::string& path, std::string_view content,
                                            bool enforce_secure_permissions) {
    auto open_result = safe_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (open_result.is_error()) {
        return Result<void>::error(open_result.error());
    }

    ScopedFd fd(open_result.value());

    if (enforce_secure_permissions && (::fchmod(fd.get(), S_IRUSR | S_IWUSR) == -1)) {
        std::string error_message =
            "Failed to set secure permissions on '" + path + "': " + describe_errno(errno);
        return Result<void>::error(error_message);
    }

    auto write_result = write_all(fd.get(), content);

    if (write_result.is_error()) {
        return Result<void>::error(write_result.error());
    }

    return Result<void>::ok();
}
}  // namespace

Result<void> write_file_content(const std::string& path, const std::string& content) {
    return write_content_with_permissions(path, std::string_view{content}, true);
}

Result<void> write_all(int fd, std::string_view data) {
    size_t total_written = 0;
    while (total_written < data.size()) {
        size_t remaining = data.size() - total_written;
#ifdef SSIZE_MAX
        remaining = std::min(remaining, static_cast<size_t>(SSIZE_MAX));
#endif
        ssize_t written = ::write(fd, data.data() + total_written, remaining);
        if (written == -1) {
            if (errno == EINTR
#ifdef EAGAIN
                || errno == EAGAIN
#endif
#ifdef EWOULDBLOCK
                || errno == EWOULDBLOCK
#endif
            ) {
                continue;
            }

            if (errno == EPIPE) {
                return Result<void>::error("Broken pipe (EPIPE)");
            }
            return Result<void>::error("Failed to write to file descriptor " + std::to_string(fd) +
                                       ": " + describe_errno(errno));
        }
        if (written == 0) {
            return Result<void>::error("Write to file descriptor " + std::to_string(fd) +
                                       " returned zero bytes");
        }
        total_written += static_cast<size_t>(written);
    }
    return Result<void>::ok();
}

bool error_indicates_broken_pipe(std::string_view message) {
    return (message.find("Broken pipe") != std::string::npos) ||
           (message.find("EPIPE") != std::string::npos);
}

std::optional<HereStringError> setup_here_string_stdin(const std::string& here_string) {
    int here_pipe[2];
    auto pipe_result = create_pipe_cloexec(here_pipe);
    if (pipe_result.is_error()) {
        return HereStringError{HereStringErrorType::Pipe, pipe_result.error()};
    }

    std::string content = here_string;
    if (g_shell && (g_shell->get_parser() != nullptr)) {
        g_shell->get_parser()->expand_env_vars(content);
    }
    content.push_back('\n');

    auto write_result = write_all(here_pipe[1], std::string_view{content});
    std::fill(content.begin(), content.end(), '\0');
    if (write_result.is_error() && !error_indicates_broken_pipe(write_result.error())) {
        close_pipe(here_pipe);
        return HereStringError{HereStringErrorType::Write, write_result.error()};
    }

    auto dup_result = duplicate_pipe_read_end_to_fd(here_pipe, STDIN_FILENO);
    if (dup_result.is_error()) {
        return HereStringError{HereStringErrorType::Dup, dup_result.error()};
    }

    return std::nullopt;
}

bool should_noclobber_prevent_overwrite(const std::string& filename, bool force_overwrite) {
    if (force_overwrite) {
        return false;
    }

    if (!g_shell || !g_shell->get_shell_option(ShellOption::Noclobber)) {
        return false;
    }

    struct stat file_stat{};
    return stat(filename.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

bool command_exists(const std::string& command_path) {
    if (command_path.empty()) {
        return false;
    }
    if (command_path.find('/') != std::string::npos) {
        return ::access(command_path.c_str(), F_OK) == 0;
    }
    return !resolve_command_with_cache(command_path, CacheUsage::Query).empty();
}

bool resolves_to_executable(const std::string& name, const std::string& cwd) {
    if (name.empty()) {
        return false;
    }

    std::filesystem::path candidate(name);

    if (name.find('/') != std::string::npos) {
        if (!candidate.is_absolute()) {
            candidate = std::filesystem::path(cwd) / candidate;
        }

        return path_is_executable(candidate);
    }

    return !resolve_command_with_cache(name, CacheUsage::Query).empty();
}

bool path_is_directory_candidate(const std::string& value, const std::string& cwd) {
    if (value.empty()) {
        return false;
    }

    std::filesystem::path candidate(value);
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::path(cwd) / candidate;
    }

    return path_is_directory(candidate);
}

bool token_has_explicit_path_hint(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    if (value[0] == '/') {
        return true;
    }

    return value.rfind("./", 0) == 0 || value.rfind("../", 0) == 0 || value.rfind("~/", 0) == 0 ||
           value.rfind("-/", 0) == 0 || value.find('/') != std::string::npos;
}

std::filesystem::path expand_shell_path_token(const std::string& value, const std::string& cwd,
                                              const std::string& previous_directory) {
    if (value.empty()) {
        return {};
    }

    if (value == "-") {
        if (previous_directory.empty()) {
            return {};
        }
        return std::filesystem::path(previous_directory);
    }

    if (value == "~" || value.rfind("~/", 0) == 0) {
        return expand_leading_tilde_path(value);
    }

    if (value.rfind("-/", 0) == 0) {
        if (previous_directory.empty()) {
            return {};
        }

        std::filesystem::path candidate(previous_directory);
        if (value.size() > 2) {
            candidate /= value.substr(2);
        }
        return candidate;
    }

    std::filesystem::path candidate(value);
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::path(cwd) / candidate;
    }

    return candidate;
}

std::string resolve_shell_token_path(const std::string& value, const std::string& cwd,
                                     const std::string& previous_directory) {
    std::filesystem::path resolved = expand_shell_path_token(value, cwd, previous_directory);
    if (resolved.empty()) {
        return value;
    }
    return resolved.lexically_normal().string();
}

bool is_directory_path(const std::filesystem::path& path) {
    return path_is_directory(path);
}

std::string resolve_existing_shell_directory_token(const std::string& value, const std::string& cwd,
                                                   const std::string& previous_directory) {
    if (value.empty()) {
        return {};
    }

    std::filesystem::path candidate = expand_shell_path_token(value, cwd, previous_directory);
    if (candidate.empty()) {
        return {};
    }

    if (!is_directory_path(candidate)) {
        return {};
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(candidate, ec);
    if (!ec && !canonical.empty()) {
        return canonical.string();
    }

    return candidate.lexically_normal().string();
}

bool is_auto_cd_directory_token(const std::string& value, const std::string& cwd,
                                const std::string& previous_directory) {
    if (value.empty()) {
        return false;
    }

    std::filesystem::path candidate = expand_shell_path_token(value, cwd, previous_directory);
    if (candidate.empty()) {
        return false;
    }

    return is_directory_path(candidate);
}

Result<std::string> read_file_content(const std::string& path, bool require_regular_file) {
    const int flags = O_RDONLY | (require_regular_file ? O_NONBLOCK | O_NOCTTY | O_CLOEXEC : 0);
    auto open_result = safe_open(path, flags);
    if (open_result.is_error()) {
        return Result<std::string>::error(open_result.error());
    }

    ScopedFd fd(open_result.value());
    if (require_regular_file) {
        struct stat status{};
        if (fstat(fd.get(), &status) != 0) {
            return Result<std::string>::error("Failed to inspect file '" + path +
                                              "': " + describe_errno(errno));
        }
        if (!S_ISREG(status.st_mode)) {
            return Result<std::string>::error("Not a regular file: '" + path + "'");
        }
    }
    std::string content;
    char buffer[4096];

    while (true) {
        ssize_t bytes_read = ::read(fd.get(), buffer, sizeof(buffer));
        if (bytes_read > 0) {
            (void)content.append(buffer, static_cast<size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        return Result<std::string>::error("Failed to read from file '" + path +
                                          "': " + describe_errno(errno));
    }

    return Result<std::string>::ok(content);
}

std::vector<std::string> get_executables_in_path() {
    return get_executables_from_path_cache();
}

bool file_exists(const std::filesystem::path& path) {
    return path_exists(path);
}

namespace {
void warn_persistence_unavailable(const std::filesystem::path& path) {
    print_error({ErrorType::RUNTIME_ERROR,
                 ErrorSeverity::WARNING,
                 path.string(),
                 "persistence unavailable; continuing without this storage",
                 {}});
}

bool prepare_persistence_directory(const std::filesystem::path& path) {
    std::error_code ec;
    (void)std::filesystem::create_directories(path, ec);
    return !ec && access(path.c_str(), W_OK | X_OK) == 0;
}
}  // namespace

bool initialize_cjsh_directories() {
    static bool initialized = false;
    if (initialized) {
        return true;
    }
    initialized = true;

    // Never create a missing HOME as a side effect of starting a shell.
    const bool home_exists = path_is_directory(g_user_home_path());
    const bool cache_ok = home_exists && prepare_persistence_directory(g_cjsh_cache_path());
    config::cache_persistence_enabled = cache_ok;
    if (!cache_ok) {
        warn_persistence_unavailable(g_cjsh_cache_path());
    }

    if (config::completion_learning_enabled &&
        (!cache_ok || !prepare_persistence_directory(g_cjsh_generated_completions_path()))) {
        config::completion_learning_enabled = false;
        if (cache_ok) {
            warn_persistence_unavailable(g_cjsh_generated_completions_path());
        }
    }
    return true;
}

void initialize_history_storage() {
    if (!config::history_enabled) {
        return;
    }
    (void)initialize_cjsh_directories();
    const auto& path = g_cjsh_history_path();
    static std::optional<std::filesystem::path> prepared_path;
    if (prepared_path && *prepared_path == path) {
        return;
    }
    prepared_path = path;

    // An early history command may have prepared a different path before .cjshrc.
    const bool custom = !history_path_state().override_value->empty();
    const bool cache_ok = config::cache_persistence_enabled;
    const bool directory_ok =
        (custom || cache_ok) && prepare_persistence_directory(path.parent_path());
    int fd = directory_ok
                 ? open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0600)
                 : -1;
    struct stat history_stat{};
    config::history_persistence_enabled =
        fd >= 0 && fstat(fd, &history_stat) == 0 && S_ISREG(history_stat.st_mode);
    close_fd_if_valid(fd);
    if (!config::history_persistence_enabled && (custom || cache_ok)) {
        warn_persistence_unavailable(path);
    }
}

std::string find_executable_in_path(const std::string& name) {
    return resolve_command_with_cache(name, CacheUsage::Query);
}

std::string resolve_executable_for_execution(const std::string& name) {
    return resolve_command_with_cache(name, CacheUsage::Execution);
}

std::string resolve_cjsh_executable_path(const std::vector<std::string>& startup_args) {
    if (auto executable_path = resolve_running_executable_path(); executable_path.has_value()) {
        std::filesystem::path normalized = normalize_path(*executable_path);
        if (!normalized.empty()) {
            return normalized.string();
        }
    }

    if (!startup_args.empty()) {
        if (auto resolved = resolve_executable_token(startup_args.front()); resolved.has_value()) {
            return resolved->string();
        }
    }

    for (const char* variable_name : {"0", "SHELL"}) {
        std::string value = cjsh_env::get_shell_variable_value(variable_name);
        if (auto resolved = resolve_executable_token(value); resolved.has_value()) {
            return resolved->string();
        }
    }

    std::string from_path = find_executable_in_path("cjsh");
    if (!from_path.empty()) {
        return normalize_path(from_path).string();
    }

    return {};
}

std::string resolve_cjsh_executable_directory(const std::vector<std::string>& startup_args) {
    std::string executable_path = resolve_cjsh_executable_path(startup_args);
    if (executable_path.empty()) {
        return safe_current_directory();
    }

    std::filesystem::path normalized = normalize_path(executable_path);
    if (!normalized.empty() && !normalized.parent_path().empty()) {
        return normalized.parent_path().string();
    }

    std::filesystem::path fallback(executable_path);
    if (!fallback.parent_path().empty()) {
        return fallback.parent_path().lexically_normal().string();
    }

    return safe_current_directory();
}

std::string resolve_cjsh_argv0(const std::vector<std::string>& startup_args,
                               const std::string& executable_path) {
    if (!startup_args.empty()) {
        std::string cleaned = strip_login_prefix(startup_args.front());
        if (!cleaned.empty()) {
            if (cleaned.find('/') != std::string::npos) {
                std::string filename = std::filesystem::path(cleaned).filename().string();
                if (!filename.empty()) {
                    return filename;
                }
            }
            return cleaned;
        }
    }

    std::string resolved_executable = executable_path;
    if (resolved_executable.empty()) {
        resolved_executable = resolve_cjsh_executable_path(startup_args);
    }

    if (!resolved_executable.empty()) {
        std::string filename = std::filesystem::path(resolved_executable).filename().string();
        if (!filename.empty()) {
            return filename;
        }
    }

    return kDefaultCjshArgv0;
}

bool hash_executable(const std::string& name, std::string* resolved_path) {
    if (name.empty() || name.find('/') != std::string::npos) {
        if (resolved_path) {
            resolved_path->clear();
        }
        return false;
    }

    std::string resolved = resolve_command_with_cache(name, CacheUsage::Manual);
    if (resolved_path) {
        *resolved_path = resolved;
    }
    return !resolved.empty();
}

std::vector<PathHashEntry> get_path_hash_entries() {
    return get_entries_from_path_cache();
}

void reset_path_hash() {
    reset_path_cache_entries();
}

namespace {
bool write_configuration_file(const std::filesystem::path& target_path,
                              const std::string& content) {
    if (!target_path.parent_path().empty()) {
        std::error_code dir_error;
        (void)std::filesystem::create_directories(target_path.parent_path(), dir_error);
        if (dir_error) {
            print_error({ErrorType::RUNTIME_ERROR,
                         target_path.parent_path().string(),
                         "Failed to prepare configuration directory: " + dir_error.message(),
                         {"Check file permissions"}});
            return false;
        }
    }

    auto write_result = write_file_content(target_path.string(), content);

    if (!write_result.is_ok()) {
        print_error(
            {ErrorType::RUNTIME_ERROR, "", write_result.error(), {"Check file permissions"}});
        return false;
    }

    return true;
}

bool execute_startup_file_if_present(const std::filesystem::path& path, bool optional_mode) {
    if (SignalHandler::startup_interrupted()) {
        return false;
    }
    if (!path_is_regular_file(path)) {
        return false;
    }

    // Validate and read the same descriptor. A FIFO swapped in after the path
    // check must not block startup, and regular-file symlinks remain supported.
    auto content = read_file_content(path.string(), true);
    if (content.is_error()) {
        if (!optional_mode) {
            print_error({ErrorType::RUNTIME_ERROR, "source", content.error(), {}});
        }
        return false;
    }
    if (SignalHandler::startup_interrupted()) {
        return false;
    }
    std::error_code ec;
    auto source_path = std::filesystem::absolute(path, ec);
    if (ec) {
        source_path = path;
    }
    (void)g_shell->execute_script_content(content.value(), source_path.lexically_normal().string());
    return true;
}

bool process_startup_file_with_fallback(const std::filesystem::path& primary,
                                        const std::filesystem::path& alternate,
                                        bool optional_mode) {
    return execute_startup_file_if_present(primary, optional_mode) ||
           execute_startup_file_if_present(alternate, optional_mode);
}

bool create_default_startup_file(const std::filesystem::path& target_path,
                                 std::string_view file_label, std::string_view load_description) {
    std::string content;
    content.reserve(128 + file_label.size() + load_description.size());
    (void)content.append("#!/usr/bin/env cjsh\n");
    (void)content.append("# cjsh ");
    (void)content.append(file_label);
    (void)content.append("\n# ");
    (void)content.append(load_description);
    content.push_back('\n');

    return write_configuration_file(target_path, content);
}

bool startup_files_disabled() {
    return config::secure_mode || config::no_config || config::no_exec ||
           SignalHandler::startup_interrupted();
}
}  // namespace

bool create_profile_file(const std::filesystem::path& target_path) {
    return create_default_startup_file(target_path, "Configuration File",
                                       "this file is sourced when the shell starts in login mode");
}

bool create_env_file(const std::filesystem::path& target_path) {
    return create_default_startup_file(
        target_path, "Environment File",
        "this file is sourced for every shell start before login/interactive setup");
}

bool create_source_file(const std::filesystem::path& target_path) {
    return create_default_startup_file(
        target_path, "Source File",
        "this file is sourced when the shell starts in interactive mode");
}

bool create_logout_file(const std::filesystem::path& target_path) {
    return create_default_startup_file(
        target_path, "Logout File",
        "this file is sourced when the shell exits from a login session");
}

bool is_first_boot() {
    return config::cache_persistence_enabled && !file_exists(g_cjsh_first_boot_path());
}

void process_profile_files() {
    if (startup_files_disabled()) {
        return;
    }

    if (config::is_posix_mode()) {
        (void)execute_startup_file_if_present("/etc/profile", true);
        if (!cjsh_env::exit_requested()) {
            (void)execute_startup_file_if_present(g_user_home_path() / ".profile", true);
        }
        return;
    }
    (void)process_startup_file_with_fallback(g_cjsh_profile_path(), g_cjsh_profile_alt_path(),
                                             true);
}

void process_env_files() {
    if (startup_files_disabled() || config::is_posix_mode()) {
        return;
    }

    if (cjsh_env::shell_variable_is_set("CJSH_ENV")) {
        std::string env_override = cjsh_env::get_shell_variable_value("CJSH_ENV");
        if (!env_override.empty()) {
            std::filesystem::path override_path = normalize_override_path(env_override);

            (void)execute_startup_file_if_present(override_path, true);
            return;
        }
    }

    (void)process_startup_file_with_fallback(g_cjsh_env_path(), g_cjsh_env_alt_path(), true);
}

void process_posix_env_file() {
    if (!config::is_posix_mode() || !config::interactive_mode || startup_files_disabled() ||
        getuid() != geteuid() || getgid() != getegid()) {
        return;
    }
    std::string path = cjsh_env::get_shell_variable_value("ENV");
    if (path.empty()) {
        return;
    }
    // Expand parameters (and arithmetic), without splitting, globbing, tilde
    // expansion, PATH search, or evaluating ENV as a command string.
    g_shell->get_parser()->expand_env_vars(path);
    if (!path.empty() && !cjsh_env::exit_requested()) {
        (void)execute_startup_file_if_present(path, true);
    }
}

void process_logout_file() {
    if (startup_files_disabled() || config::is_posix_mode()) {
        return;
    }

    (void)process_startup_file_with_fallback(g_cjsh_logout_path(), g_cjsh_logout_alt_path(), true);
}

void process_source_files() {
    if (!config::source_enabled || startup_files_disabled() || config::is_posix_mode()) {
        return;
    }

    (void)process_startup_file_with_fallback(g_cjsh_source_path(), g_cjsh_source_alt_path(), false);
}

}  // namespace cjsh_filesystem
