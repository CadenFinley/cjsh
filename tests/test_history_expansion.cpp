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
#include "history_file_utils.h"
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
            EXPECT_TRUE(result.error_message.find("empty search") != std::string::npos, test_name,
                        "error should explain the empty search");
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

static bool test_committed_history_event_selection() {
    const char* test_name = "committed_history_event_selection";
    const std::vector<std::string> history = {"echo older alpha", "echo newest beta"};
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"!!", "echo newest beta"},
        {"echo !!", "echo echo newest beta"},
        {"!-1", "echo newest beta"},
        {"!-2", "echo older alpha"},
        {"!0", "echo older alpha"},
        {"!1", "echo newest beta"},
        {"!echo", "echo newest beta"},
        {"!?newest?", "echo newest beta"},
        {"!?older?", "echo older alpha"},
        {"^newest^changed^", "echo changed beta"},
        {"echo !$ !^ !*", "echo beta newest newest beta"},
        {"echo !!:$ !!:^ !!:*", "echo beta newest newest beta"},
        {"echo !:0 !:1-2 !:2-", "echo echo newest beta beta"},
        {"echo !-1:$ !echo:^ !?newest?:*", "echo beta newest newest beta"},
        {"!!; !!", "echo newest beta; echo newest beta"},
    };
    for (const auto& [command, expected] : cases) {
        const auto result = HistoryExpansion::expand(command, history);
        EXPECT_FALSE(result.has_error, test_name, command.c_str());
        EXPECT_TRUE(result.was_expanded && result.should_echo, test_name, command.c_str());
        if (!expect_streq(result.expanded_command, expected, test_name, command.c_str())) {
            return false;
        }
    }
    const auto single = HistoryExpansion::expand("!!", {"echo only"});
    EXPECT_FALSE(single.has_error, test_name, "a single committed entry must be usable");
    return expect_streq(single.expanded_command, "echo only", test_name,
                        "the only entry must not be discarded");
}

static bool test_committed_history_errors_preserve_input() {
    const char* test_name = "committed_history_errors_preserve_input";
    for (const std::string command :
         {"!!", "!-1", "!0", "!echo", "!?echo?", "!$", "!^", "!*", "!:1", "^old^new^"}) {
        const auto result = HistoryExpansion::expand(command, {});
        EXPECT_TRUE(result.has_error, test_name, command.c_str());
        EXPECT_FALSE(result.was_expanded, test_name, command.c_str());
        EXPECT_TRUE(result.error_message.find("event not found") != std::string::npos, test_name,
                    command.c_str());
        if (!expect_streq(result.expanded_command, command, test_name, command.c_str())) {
            return false;
        }
    }
    for (const std::string command :
         {"!-2", "!1", "!missing", "!?missing?", "!!:9", "^missing^new^"}) {
        const auto result = HistoryExpansion::expand(command, {"echo only"});
        EXPECT_TRUE(result.has_error, test_name, command.c_str());
        EXPECT_FALSE(result.was_expanded, test_name, command.c_str());
        if (!expect_streq(result.expanded_command, command, test_name, command.c_str())) {
            return false;
        }
    }
    return true;
}

static bool test_literal_history_syntax_is_not_reexpanded() {
    const char* test_name = "literal_history_syntax_is_not_reexpanded";
    for (const std::string command : {"echo '!!'", "echo \\!\\!", "echo hello!", "! true"}) {
        const auto result = HistoryExpansion::expand(command, {"echo previous"});
        EXPECT_FALSE(result.has_error || result.was_expanded || result.should_echo, test_name,
                     command.c_str());
        if (!expect_streq(result.expanded_command, command, test_name, command.c_str())) {
            return false;
        }
    }
    const auto result = HistoryExpansion::expand("!!", {"echo '!!'"});
    EXPECT_FALSE(result.has_error, test_name, "replayed text must not expand recursively");
    return expect_streq(result.expanded_command, "echo '!!'", test_name,
                        "literal bangs in the recalled command must survive");
}

static bool test_history_file_decodes_entries() {
    const char* test_name = "history_file_decodes_entries";
    const std::string content =
        "# timestamp=1 frequency=1 code=0 ms=1 cwd=%2Ftmp\n"
        "echo older\n\n"
        "# timestamp=2 frequency=1 code=127 ms=2 cwd=%2Ftmp\n"
        "  echo first\\n\\n\\techo second\n"
        "echo 'literal\\\\n' 'C:\\\\tmp'\\t\\x23tag café\n"
        "echo final";
    const auto entries = history_file_utils::parse_history_entries(content);
    const std::vector<std::string> expected = {
        "echo older",
        "  echo first\n\n\techo second",
        "echo 'literal\\n' 'C:\\tmp'\t#tag café",
        "echo final",
    };
    EXPECT_TRUE(entries == expected, test_name,
                "metadata and blank lines must be skipped and commands decoded exactly once");
    const auto replay = HistoryExpansion::expand("!-3", entries);
    EXPECT_FALSE(replay.has_error, test_name, "multiline history should expand");
    return expect_streq(replay.expanded_command, expected[1], test_name,
                        "a multiline command must remain one event with real newlines");
}

static bool test_history_file_rejects_malformed_entries() {
    const char* test_name = "history_file_rejects_malformed_entries";
    const auto entries = history_file_utils::parse_history_entries(
        "echo valid\ninvalid\\q\ninvalid\\xZZ\ntruncated\\\n\\r\necho last\n");
    EXPECT_TRUE(entries == std::vector<std::string>({"echo valid", "echo last"}), test_name,
                "invalid escapes and empty decoded records must not become history events");
    EXPECT_TRUE(history_file_utils::parse_history_entries("\n# metadata only\n").empty(), test_name,
                "a file without commands must produce empty history");
    return true;
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
        {"committed_history_event_selection", test_committed_history_event_selection},
        {"committed_history_errors_preserve_input", test_committed_history_errors_preserve_input},
        {"literal_history_syntax_is_not_reexpanded", test_literal_history_syntax_is_not_reexpanded},
        {"history_file_decodes_entries", test_history_file_decodes_entries},
        {"history_file_rejects_malformed_entries", test_history_file_rejects_malformed_entries},
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
