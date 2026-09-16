/*
  test_history_expansion.cpp

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

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "history_expansion.h"
#include "shell.h"

std::unique_ptr<Shell> g_shell;

static void log_failure(const char* test_name, const char* message) {
    (void)std::fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
}

#define EXPECT_TRUE(condition, test_name, message) \
    do {                                           \
        if (!(condition)) {                        \
            log_failure(test_name, message);       \
            return false;                          \
        }                                          \
    } while (0)

#define EXPECT_FALSE(condition, test_name, message) EXPECT_TRUE(!(condition), test_name, message)

static bool expect_streq(const std::string& actual, const std::string& expected,
                         const char* test_name, const char* message) {
    if (actual == expected) {
        return true;
    }

    (void)std::fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
    (void)std::fprintf(stderr, "  actual:   %s\n", actual.c_str());
    (void)std::fprintf(stderr, "  expected: %s\n", expected.c_str());
    return false;
}

static bool test_substring_search_skips_staged_entry() {
    const char* test_name = "substring_search_skips_staged_entry";
    const std::vector<std::string> history = {"echo alpha", "git status", "!?status?"};

    const auto result = HistoryExpansion::expand("!?status?", history, true);

    EXPECT_FALSE(result.has_error, test_name, "substring search should not fail");
    EXPECT_TRUE(result.was_expanded, test_name, "substring search should expand");
    return expect_streq(result.expanded_command, "git status", test_name,
                        "substring search should use the previous matching command");
}

static bool test_quick_substitution_skips_staged_entry() {
    const char* test_name = "quick_substitution_skips_staged_entry";
    const std::vector<std::string> history = {"echo alpha beta", "^beta^gamma"};

    const auto result = HistoryExpansion::expand("^beta^gamma", history, true);

    EXPECT_FALSE(result.has_error, test_name, "quick substitution should not fail");
    EXPECT_TRUE(result.was_expanded, test_name, "quick substitution should expand");
    return expect_streq(result.expanded_command, "echo alpha gamma", test_name,
                        "quick substitution should target the previous command");
}

static bool test_quick_substitution_rejects_empty_search() {
    const char* test_name = "quick_substitution_rejects_empty_search";
    const std::vector<std::string> commands = {
        "^^", "^^^", "^^^^^^^^^^^", std::string(39, '^'), "^^replacement^",
    };
    for (const auto& command : commands) {
        for (bool staged : {false, true}) {
            std::vector<std::string> history = {"clear"};
            if (staged) {
                history.push_back(command);
            }
            const auto result = HistoryExpansion::expand(command, history, staged);
            EXPECT_TRUE(result.has_error, test_name, "empty search must not replay history");
            EXPECT_FALSE(result.was_expanded, test_name, "invalid input must not expand");
            EXPECT_TRUE(result.error_message.find("empty search") != std::string::npos,
                        test_name, "error should explain the empty search");
            if (!expect_streq(result.expanded_command, command, test_name,
                              "invalid input must remain unchanged")) {
                return false;
            }
        }
    }
    return true;
}

static bool test_quick_substitution_valid_forms() {
    const char* test_name = "quick_substitution_valid_forms";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"^alpha^gamma", "echo gamma alpha beta"},
        {"^alpha^gamma^", "echo gamma alpha beta"},
        {"^alpha^", "echo  alpha beta"},
        {"^alpha^^", "echo  alpha beta"},
        {"^alpha^alpha^", "echo alpha alpha beta"},
    };
    for (const auto& [command, expected] : cases) {
        const auto result = HistoryExpansion::expand(command, {"echo alpha alpha beta"});
        EXPECT_FALSE(result.has_error, test_name, "valid substitution should succeed");
        EXPECT_TRUE(result.was_expanded, test_name, "valid substitution should expand");
        if (!expect_streq(result.expanded_command, expected, test_name,
                          "only the first match should be replaced")) {
            return false;
        }
    }
    return true;
}

static bool test_quick_substitution_preserves_suffix() {
    const char* test_name = "quick_substitution_preserves_suffix";
    for (const std::string suffix : {" extra", " && echo done", "^^"}) {
        const auto result = HistoryExpansion::expand("^alpha^beta^" + suffix, {"echo alpha"});
        EXPECT_FALSE(result.has_error, test_name, "substitution with suffix should succeed");
        EXPECT_TRUE(result.was_expanded, test_name, "substitution with suffix should expand");
        if (!expect_streq(result.expanded_command, "echo beta" + suffix, test_name,
                          "text after the closing caret must not be discarded")) {
            return false;
        }
    }
    return true;
}

static bool test_quick_substitution_errors() {
    const char* test_name = "quick_substitution_errors";
    const auto missing_history = HistoryExpansion::expand("^alpha^beta^", {});
    EXPECT_TRUE(missing_history.has_error, test_name, "missing history should fail");
    EXPECT_FALSE(missing_history.was_expanded, test_name, "missing history must not expand");
    const auto missing_match = HistoryExpansion::expand("^missing^beta^", {"echo alpha"});
    EXPECT_TRUE(missing_match.has_error, test_name, "missing match should fail");
    EXPECT_FALSE(missing_match.was_expanded, test_name, "missing match must not expand");
    return true;
}

static bool test_previous_command_word_designators_expand() {
    const char* test_name = "previous_command_word_designators_expand";
    const std::vector<std::string> history = {"cp source.txt dest.txt", "!$"};

    const auto last_arg = HistoryExpansion::expand("!$", history, true);
    EXPECT_FALSE(last_arg.has_error, test_name, "!$ should not fail");
    EXPECT_TRUE(last_arg.was_expanded, test_name, "!$ should expand");
    if (!expect_streq(last_arg.expanded_command, "dest.txt", test_name,
                      "!$ should expand to the previous command's last argument")) {
        return false;
    }

    const auto first_arg = HistoryExpansion::expand("!^", history, true);
    EXPECT_FALSE(first_arg.has_error, test_name, "!^ should not fail");
    EXPECT_TRUE(first_arg.was_expanded, test_name, "!^ should expand");
    if (!expect_streq(first_arg.expanded_command, "source.txt", test_name,
                      "!^ should expand to the previous command's first argument")) {
        return false;
    }

    const auto all_args = HistoryExpansion::expand("!*", history, true);
    EXPECT_FALSE(all_args.has_error, test_name, "!* should not fail");
    EXPECT_TRUE(all_args.was_expanded, test_name, "!* should expand");
    return expect_streq(all_args.expanded_command, "source.txt dest.txt", test_name,
                        "!* should expand to all previous command arguments");
}

static bool test_double_bang_replays_last_expanded_command() {
    const char* test_name = "double_bang_replays_last_expanded_command";
    const std::vector<std::string> history = {"echo alpha", "echo alpha", "!!"};

    const auto result = HistoryExpansion::expand("!!", history, true);

    EXPECT_FALSE(result.has_error, test_name, "double bang should not fail");
    EXPECT_TRUE(result.was_expanded, test_name, "double bang should expand");
    return expect_streq(result.expanded_command, "echo alpha", test_name,
                        "double bang should replay the previous expanded command");
}

int main() {
    struct TestCase {
        const char* name;
        bool (*func)();
    };

    const TestCase tests[] = {
        {"substring_search_skips_staged_entry", test_substring_search_skips_staged_entry},
        {"quick_substitution_skips_staged_entry", test_quick_substitution_skips_staged_entry},
        {"quick_substitution_rejects_empty_search", test_quick_substitution_rejects_empty_search},
        {"quick_substitution_valid_forms", test_quick_substitution_valid_forms},
        {"quick_substitution_preserves_suffix", test_quick_substitution_preserves_suffix},
        {"quick_substitution_errors", test_quick_substitution_errors},
        {"previous_command_word_designators_expand", test_previous_command_word_designators_expand},
        {"double_bang_replays_last_expanded_command",
         test_double_bang_replays_last_expanded_command},
    };

    const std::size_t test_count = sizeof(tests) / sizeof(tests[0]);
    std::size_t failures = 0;
    bool all_passed = true;
    for (const auto& test : tests) {
        if (!test.func()) {
            all_passed = false;
            ++failures;
        }
    }

    if (!all_passed) {
        (void)std::fprintf(stderr, "%zu/%zu history expansion tests failed\n", failures,
                           test_count);
        return 1;
    }

    (void)std::printf("All %zu history expansion tests passed\n", test_count);
    return 0;
}
