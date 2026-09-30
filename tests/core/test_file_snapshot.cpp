/*
  test_file_snapshot.cpp

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

#include <chrono>
#include <cstdio>

#include "file_snapshot.h"

int main() {
    using cjsh_filesystem::FileSnapshot;
    using Clock = std::chrono::system_clock;
    using std::chrono::seconds;
    size_t checks = 0;
    size_t failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        ++checks;
        if (!condition) {
            ++failures;
            (void)std::fprintf(stderr, "[FAIL] %s\n", message);
        }
    };

    // Model two same-size writes in one filesystem clock tick. The second
    // observation can happen much later without revealing the missed write.
    for (const auto mode : {S_IFREG, S_IFDIR}) {
        FileSnapshot original;
        original.info.st_mode = mode | 0700;
        original.info.st_size = 4096;
        original.info.st_mtime = 100;
        original.info.st_ctime = 100;
        original.observed_at = Clock::time_point(seconds(100));
        auto current = original;
        expect(original == current, "matching metadata remains equal");
        expect(!original.can_reuse_cached_data(current),
               "same-tick writes cannot be ruled out by equal timestamps");
        current.observed_at += seconds(10);
        expect(!original.can_reuse_cached_data(current),
               "a potentially stale observation must not become reusable just by waiting");
        const auto refreshed = current;
        current.observed_at += seconds(1);
        expect(refreshed.can_reuse_cached_data(current),
               "data reread after timestamps settle can be cached");

        auto modified = current;
        ++modified.info.st_ino;
        expect(!refreshed.can_reuse_cached_data(modified),
               "an atomic replacement invalidates settled data");
        modified = current;
        ++modified.info.st_size;
        expect(!refreshed.can_reuse_cached_data(modified), "appends invalidate settled data");
        modified = current;
        ++modified.info.st_ctime;
        expect(!refreshed.can_reuse_cached_data(modified),
               "metadata changes invalidate settled data");

        auto recent_change = current;
        recent_change.info.st_ctime = 111;
        expect(!recent_change.can_reuse_cached_data(recent_change),
               "an old mtime does not hide a recent metadata change");
        auto recent_write = current;
        recent_write.info.st_mtime = 111;
        expect(!recent_write.can_reuse_cached_data(recent_write),
               "an old ctime does not hide a recent write");
        auto future = current;
        future.info.st_mtime = 200;
        expect(!future.can_reuse_cached_data(future),
               "future timestamps after a clock adjustment are not reusable");
    }

    FileSnapshot missing;
    missing.error = ENOENT;
    expect(missing.can_reuse_cached_data(missing), "unchanged missing files can be cached");
    FileSnapshot present;
    expect(!missing.can_reuse_cached_data(present), "newly created files invalidate misses");
    expect(!present.can_reuse_cached_data(missing), "removed files invalidate cached data");

    (void)std::printf("File snapshot tests: %zu/%zu passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
