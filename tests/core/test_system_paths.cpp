/*
  test_system_paths.cpp

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

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cjsh_filesystem.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"

std::unique_ptr<Shell> g_shell;

namespace {

bool expect_path(const char* expected, const char* message) {
    const char* actual = getenv("PATH");
    if (expected == nullptr ? actual == nullptr : actual && std::string(actual) == expected) {
        return true;
    }
    (void)std::fprintf(stderr, "[FAIL] %s\nExpected: %s\nActual: %s\n", message,
                       expected ? expected : "<unset>", actual ? actual : "<unset>");
    return false;
}

bool test_interactive_path_cache(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using namespace cjsh_filesystem;
    const auto first = root / "first";
    const auto second = root / "second";
    fs::create_directory(first);
    fs::create_directory(second);
    auto executable = [](const fs::path& path) {
        std::ofstream(path) << "#!/bin/sh\nexit 0\n";
        fs::permissions(path, fs::perms::owner_all);
    };
    executable(first / "tool");
    executable(second / "tool");
    std::ofstream(first / "plain") << "not executable\n";
    fs::create_directory(first / "directory");
    fs::create_symlink(first / "tool", first / "linked");
    fs::create_symlink(first / "absent", first / "broken");
    const std::string path = first.string() + ":" + second.string() + ":" + first.string();
    (void)setenv("PATH", path.c_str(), 1);
    reset_path_hash();
    bool ok = true;
    auto expect = [&](bool condition, const char* message) {
        if (!condition) {
            (void)std::fprintf(stderr, "[FAIL] %s\n", message);
            ok = false;
        }
    };
    auto interactive = [](const std::string& name) {
        const ScopedInteractivePathLookup scope;
        return find_executable_in_path(name);
    };

    expect(interactive("t").empty() && interactive("to").empty(),
           "incomplete PATH names should remain unknown");
    expect(interactive("tool") == (first / "tool").string(),
           "interactive lookup must preserve PATH precedence");
    expect(interactive("linked") == (first / "linked").string(),
           "interactive lookup must follow executable symlinks");
    expect(interactive("broken").empty() && interactive("plain").empty() &&
               interactive("directory").empty(),
           "interactive lookup must reject broken links, nonexecutables, and directories");

    fs::remove(first / "tool");
    expect(interactive("tool") == (first / "tool").string(),
           "redraws should reuse successful lookups during the same prompt");
    expect(find_executable_in_path("tool") == (second / "tool").string(),
           "ordinary queries must revalidate paths even after interactive lookups");
    reset_interactive_path_cache();
    expect(interactive("tool") == (second / "tool").string(),
           "the next prompt must discard cached lookup results");

    executable(first / "new-tool");
    {
        const ScopedInteractivePathLookup scope;
        expect(resolve_executable_for_execution("new-tool") == (first / "new-tool").string(),
               "execution must find new commands even while an interactive scope is active");
    }
    fs::permissions(first / "plain", fs::perms::owner_all);
    reset_interactive_path_cache();
    expect(interactive("plain") == (first / "plain").string(),
           "the next prompt must observe permission changes");
    executable(first / "new-after-prompt");
    reset_interactive_path_cache();
    expect(interactive("new-after-prompt") == (first / "new-after-prompt").string(),
           "the next prompt must discover commands created since the last listing");

    (void)setenv("PATH", second.c_str(), 1);
    expect(interactive("plain").empty(), "PATH changes must invalidate interactive results");
    if (getuid() != 0) {
        fs::permissions(second, fs::perms::owner_exec);
        reset_path_hash();
        const bool found_without_listing = interactive("tool") == (second / "tool").string();
        const auto candidates = get_path_completion_candidates();
        fs::permissions(second, fs::perms::owner_all);
        expect(found_without_listing,
               "searchable PATH directories must work even without permission to list names");
        expect(std::find(candidates.begin(), candidates.end(), "tool") != candidates.end(),
               "completion candidates must retain known commands in unlistable directories");
    }
    const auto previous_cwd = fs::current_path();
    fs::current_path(first);
    (void)setenv("PATH", ".", 1);
    expect(interactive("tool").empty(), "relative PATH must use the current directory");
    fs::current_path(second);
    expect(interactive("tool") == "./tool", "changing cwd must invalidate relative PATH misses");
    fs::current_path(first);
    expect(interactive("tool").empty(), "changing cwd must invalidate relative PATH hits");
    fs::current_path(previous_cwd);

    (void)setenv("PATH", "", 1);
    expect(interactive("tool").empty(), "empty PATH must discard previous interactive results");
    reset_path_hash();
    return ok;
}

}  // namespace

int main() {
    char temporary[] = "/tmp/cjsh-system-paths-XXXXXX";
    if (mkdtemp(temporary) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    const std::filesystem::path root(temporary);
    const std::string file = (root / "paths").string();
    const std::string directory = (root / "paths.d").string();
    auto setup = [&] { cjsh_env::setup_path_variables(file, directory); };
    bool ok = true;

    (void)unsetenv("PATH");
    setup();
    ok = expect_path("/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                     "missing system files and PATH should supply a working default") &&
         ok;
    (void)setenv("PATH", "", 1);
    setup();
    ok = expect_path("/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                     "an empty PATH should also receive defaults") &&
         ok;
    (void)setenv("PATH", "/custom/bin:/custom/bin-extra", 1);
    setup();
    ok = expect_path("/custom/bin:/custom/bin-extra",
                     "missing system files should retain a supplied PATH") &&
         ok;

    std::filesystem::create_directory(directory);
    std::ofstream(file) << "\n # comment\r\n /system/bin \r\n/system/bin\n/system/sbin\n";
    // Create files out of order; only their names determine loading order.
    std::ofstream(root / "paths.d/20-tools") << "/tools/bin\n/system/bin\n";
    std::ofstream(root / "paths.d/10-vendor")
        << "/vendor/bin:/vendor/sbin\n/path with spaces/bin\n$HOME/bin\n";
    std::ofstream(root / "paths.d/30-last") << "/last/bin";
    std::ofstream(root / "paths.d/.hidden") << "/hidden/bin\n";
    std::filesystem::create_directory(root / "paths.d/subdirectory");
    std::ofstream(root / "paths.d/subdirectory/ignored") << "/nested/bin\n";
    std::filesystem::create_symlink(root / "missing", root / "paths.d/broken-link");
    std::ofstream(root / "linked-paths") << "/linked/bin\n";
    std::filesystem::create_symlink(root / "linked-paths", root / "paths.d/40-link");
    if (mkfifo((root / "paths.d/fifo").c_str(), 0600) != 0) {
        std::perror("mkfifo");
        ok = false;
    }
    const std::string configured =
        "/system/bin:/system/sbin:/vendor/bin:/vendor/sbin:"
        "/path with spaces/bin:$HOME/bin:/tools/bin:/last/bin:/linked/bin";
    const char* inherited = ":/custom/bin:/system/bin::/custom/bin:/custom/bin-extra:";
    (void)setenv("PATH", inherited, 1);
    setup();
    ok = expect_path(inherited,
                     "non-login startup must preserve supplied PATH even with system files") &&
         ok;
    config::login_mode = true;
    setup();
    const std::string merged = configured + ":/custom/bin:/custom/bin-extra";
    ok = expect_path(merged.c_str(),
                     "system entries should precede unique inherited entries in file order") &&
         ok;
    setup();
    ok = expect_path(merged.c_str(), "repeated startup should not grow or reorder PATH") && ok;
    for (bool login : {false, true}) {
        config::login_mode = login;
        for (const char* value : {static_cast<const char*>(nullptr), ""}) {
            if (value) {
                (void)setenv("PATH", value, 1);
            } else {
                (void)unsetenv("PATH");
            }
            setup();
            ok =
                expect_path(configured.c_str(),
                            "login and non-login shells must initialize an empty or absent PATH") &&
                ok;
        }
    }

    config::minimal_mode = true;
    (void)setenv("PATH", inherited, 1);
    setup();
    ok = expect_path(merged.c_str(), "minimal login shells must still load system paths") && ok;
    (void)unsetenv("PATH");
    cjsh_env::setup_path_variables((root / "missing-paths").string(),
                                   (root / "missing-directory").string());
    ok = expect_path("/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                     "minimal shells must still supply a working default without system files") &&
         ok;
    config::minimal_mode = false;

    for (bool* bypass : {&config::no_system_paths, &config::no_config, &config::secure_mode,
                         static_cast<bool*>(nullptr), &config::no_exec}) {
        if (bypass) {
            *bypass = true;
        } else {
            config::set_shell_dialect(config::ShellDialect::Posix);
        }
        for (const char* value : {static_cast<const char*>(nullptr), "", ":/custom::/custom:"}) {
            if (value) {
                (void)setenv("PATH", value, 1);
            } else {
                (void)unsetenv("PATH");
            }
            setup();
            ok = expect_path(value, "bypass modes must preserve PATH exactly") && ok;
        }
        if (bypass) {
            *bypass = false;
        } else {
            config::set_shell_dialect(config::ShellDialect::Cjsh);
        }
    }

    // Missing or unusable inputs must not prevent loading the other source.
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    std::ofstream(root / "only-paths") << "/file-only/bin\n";
    (void)setenv("PATH", "/inherited/bin", 1);
    cjsh_env::setup_path_variables((root / "only-paths").string(), file);
    ok =
        expect_path("/file-only/bin:/inherited/bin", "a non-directory paths.d should be skipped") &&
        ok;
    (void)setenv("PATH", "/inherited/bin", 1);
    cjsh_env::setup_path_variables(file, (root / "missing-directory").string());
    ok = expect_path("/inherited/bin", "a non-file paths input should be skipped") && ok;
    if (getuid() != 0) {
        const auto unreadable = root / "only-paths";
        (void)chmod(unreadable.c_str(), 0000);
        cjsh_env::setup_path_variables(unreadable.string(), (root / "missing-directory").string());
        ok = expect_path("/inherited/bin", "an unreadable paths file should be skipped") && ok;
        (void)chmod(unreadable.c_str(), 0600);
    }

    ok = test_interactive_path_cache(root) && ok;
    std::filesystem::remove_all(root);
    return ok ? 0 : 1;
}
