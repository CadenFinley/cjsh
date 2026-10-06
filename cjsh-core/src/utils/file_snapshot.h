/*
  file_snapshot.h

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

#ifndef CJSH_CORE_SRC_UTILS_FILE_SNAPSHOT_H
#define CJSH_CORE_SRC_UTILS_FILE_SNAPSHOT_H

#include <sys/stat.h>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <tuple>

namespace cjsh_filesystem {

// Identity as well as nanosecond timestamps matters: history writers and command
// installers often replace a file atomically with another of the same size.
struct FileSnapshot {
    struct stat info{};
    int error = 0;
    std::chrono::system_clock::time_point observed_at;

    static FileSnapshot read(const std::filesystem::path& path) {
        FileSnapshot snapshot;
        snapshot.observed_at = std::chrono::system_clock::now();
        if (::stat(path.c_str(), &snapshot.info) != 0) {
            snapshot.error = errno;
        }
        return snapshot;
    }

    bool operator==(const FileSnapshot& other) const {
        if (error != 0 || other.error != 0) {
            return error == other.error;
        }
#if defined(__APPLE__)
        const auto modified = info.st_mtimespec;
        const auto changed = info.st_ctimespec;
        const auto other_modified = other.info.st_mtimespec;
        const auto other_changed = other.info.st_ctimespec;
#else
        const auto modified = info.st_mtim;
        const auto changed = info.st_ctim;
        const auto other_modified = other.info.st_mtim;
        const auto other_changed = other.info.st_ctim;
#endif
        return std::tie(info.st_dev, info.st_ino, info.st_size, info.st_mode, modified.tv_sec,
                        modified.tv_nsec, changed.tv_sec, changed.tv_nsec) ==
               std::tie(other.info.st_dev, other.info.st_ino, other.info.st_size,
                        other.info.st_mode, other_modified.tv_sec, other_modified.tv_nsec,
                        other_changed.tv_sec, other_changed.tv_nsec);
    }

    bool operator!=(const FileSnapshot& other) const {
        return !(*this == other);
    }

    bool can_reuse_cached_data(const FileSnapshot& current) const {
        if (*this != current) {
            return false;
        }
        if (error != 0) {
            return true;
        }
        // Nanosecond fields can still use a coarse filesystem/kernel clock.
        // Rapid writes may leave both timestamps unchanged. Only reuse data
        // observed after that timestamp's resolution window has passed. Use
        // the original observation time: waiting cannot make stale data safe.
        const auto stable_before = observed_at.time_since_epoch() - std::chrono::seconds(2);
        return std::chrono::seconds(info.st_mtime) < stable_before &&
               std::chrono::seconds(info.st_ctime) < stable_before;
    }
};

}  // namespace cjsh_filesystem

#endif
