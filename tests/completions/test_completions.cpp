/*
  test_completions.cpp

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

#include <sys/types.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "builtins_completions_handler.h"
#include "cjsh_completions.h"
#include "cjsh_filesystem.h"
#include "command_line_utils.h"
#include "completion_context.h"
#include "completion_spec.h"
#include "completion_spell.h"
#include "completion_tracker.h"
#include "completion_utils.h"
#include "external_sub_completions.h"
#include "isocline/isocline.h"
#include "shell.h"
#include "shell_env.h"
extern "C" {
#include "common.h"
#include "completions.h"
#include "env.h"
#include "stringbuf.h"
#include "tty.h"
}

static void log_failure(const char* test_name, const char* message) {
    (void)std::fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
}

#define EXPECT_TRUE(condition, test_name, message)    \
    do {                                              \
        const bool cjsh_test_condition = (condition); \
        if (!cjsh_test_condition) {                   \
            log_failure(test_name, message);          \
            return false;                             \
        }                                             \
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

struct CompletionAction {
    std::string text;
    long delete_before;
    long delete_after;
    const char* source;
};

static std::vector<CompletionAction> g_completion_actions;
static const std::unordered_map<std::string, completion_spell::SpellCorrectionMatch>*
    g_spell_matches = nullptr;
static size_t g_spell_prefix_len = 0;

static void completion_action_completer(ic_completion_env_t* cenv, const char* prefix) {
    completion_tracker::completion_session_begin(cenv, prefix);
    for (const auto& action : g_completion_actions) {
        const char* source = action.source == nullptr ? "test" : action.source;
        (void)completion_tracker::safe_add_completion_prim_with_source(
            cenv, action.text.c_str(), nullptr, nullptr, source, action.delete_before,
            action.delete_after);
    }
    completion_tracker::completion_session_end();
}

static void spell_match_completer(ic_completion_env_t* cenv, const char* prefix) {
    if (g_spell_matches == nullptr) {
        return;
    }
    completion_tracker::completion_session_begin(cenv, prefix);
    completion_spell::add_spell_correction_matches(cenv, *g_spell_matches, g_spell_prefix_len);
    completion_tracker::completion_session_end();
}

static ssize_t run_completion_generation_at(const char* input, ssize_t cursor,
                                            ic_completer_fun_t* completer, ssize_t max_results) {
    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return -1;
    }
    completions_set_completer(env->completions, completer, nullptr);
    return completions_generate(env, env->completions, input, cursor, max_results);
}

static ssize_t run_completion_generation(const char* input, ic_completer_fun_t* completer,
                                         ssize_t max_results) {
    return run_completion_generation_at(input, static_cast<ssize_t>(std::strlen(input)), completer,
                                        max_results);
}

static ssize_t run_hint_generation(const char* input) {
    ic_env_t* env = ic_get_env();
    completions_set_completer(env->completions, &cjsh_default_completer, nullptr);
    return completions_generate_hint(env, env->completions, input,
                                     static_cast<ssize_t>(std::strlen(input)), 2);
}

class ScopedEnvironmentValue {
   public:
    ScopedEnvironmentValue(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = getenv(name)) {
            previous_ = previous;
        }
        (void)setenv(name, value.c_str(), 1);
    }
    ~ScopedEnvironmentValue() {
        if (previous_) {
            (void)setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
        cjsh_filesystem::reset_path_hash();
    }

   private:
    std::string name_;
    std::optional<std::string> previous_;
};

static bool generated_completions_include_replacement(const char* replacement) {
    if (replacement == nullptr) {
        return false;
    }

    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return false;
    }

    ssize_t count = completions_count(env->completions);
    for (ssize_t i = 0; i < count; ++i) {
        const char* candidate = completions_get_replacement(env->completions, i);
        if (candidate != nullptr && std::strcmp(candidate, replacement) == 0) {
            return true;
        }
    }

    return false;
}

static std::vector<std::string> generated_completion_replacements() {
    std::vector<std::string> replacements;
    ic_env_t* env = ic_get_env();
    if (env != nullptr && env->completions != nullptr) {
        for (ssize_t i = 0; i < completions_count(env->completions); ++i) {
            replacements.emplace_back(completions_get_replacement(env->completions, i));
        }
    }
    return replacements;
}

static bool write_completion_history(const std::string& content) {
    std::ofstream history_file(cjsh_filesystem::g_cjsh_history_path());
    history_file << content;
    history_file.close();
    return history_file.good();
}

static bool first_generated_completion_matches(const char* replacement, const char* source) {
    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return false;
    }

    const char* actual_replacement = completions_get_replacement(env->completions, 0);
    const char* actual_source = completions_get_source(env->completions, 0);
    return actual_replacement != nullptr && actual_source != nullptr &&
           std::strcmp(actual_replacement, replacement) == 0 &&
           std::strcmp(actual_source, source) == 0;
}

static bool generated_completions_include_source(const char* source) {
    if (source == nullptr) {
        return false;
    }

    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return false;
    }

    ssize_t count = completions_count(env->completions);
    for (ssize_t i = 0; i < count; ++i) {
        const char* candidate_source = completions_get_source(env->completions, i);
        if (candidate_source != nullptr && std::strcmp(candidate_source, source) == 0) {
            return true;
        }
    }

    return false;
}

static ssize_t generated_completion_index_with_source(const char* source) {
    if (source == nullptr) {
        return -1;
    }

    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return -1;
    }

    ssize_t count = completions_count(env->completions);
    for (ssize_t i = 0; i < count; ++i) {
        const char* candidate_source = completions_get_source(env->completions, i);
        if (candidate_source != nullptr && std::strcmp(candidate_source, source) == 0) {
            return i;
        }
    }

    return -1;
}

static void clear_generated_completions() {
    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return;
    }
    completions_clear(env->completions);
}

static bool apply_single_generated_completion(const char* input, ssize_t cursor,
                                              const CompletionAction& action, std::string& result,
                                              ssize_t& new_pos) {
    g_completion_actions = {action};
    ssize_t count = run_completion_generation_at(input, cursor, &completion_action_completer, 64);
    g_completion_actions.clear();
    if (count != 1) {
        return false;
    }

    ic_env_t* env = ic_get_env();
    if (env == nullptr || env->completions == nullptr) {
        return false;
    }

    stringbuf_t* buffer = sbuf_new(env->mem);
    if (buffer == nullptr) {
        return false;
    }

    sbuf_replace(buffer, input);
    new_pos = completions_apply(env->completions, 0, buffer, cursor);
    result = sbuf_string(buffer);
    sbuf_free(buffer);
    completions_clear(env->completions);
    return new_pos >= 0;
}

static bool test_quote_and_unquote_paths() {
    const char* test_name = "quote_and_unquote_paths";

    std::string plain = "simple";
    if (!expect_streq(completion_utils::quote_path_if_needed(plain), "simple", test_name,
                      "plain path should be unchanged")) {
        return false;
    }

    std::string spaced = "two words";
    if (!expect_streq(completion_utils::quote_path_if_needed(spaced), "\"two words\"", test_name,
                      "paths with spaces should be quoted")) {
        return false;
    }

    std::string with_quotes = "a\"b\\c";
    if (!expect_streq(completion_utils::quote_path_if_needed(with_quotes), "\"a\\\"b\\\\c\"",
                      test_name, "quotes and backslashes should be escaped")) {
        return false;
    }

    if (!expect_streq(completion_utils::unquote_path("\"two words\""), "two words", test_name,
                      "double-quoted path should be unquoted")) {
        return false;
    }

    if (!expect_streq(completion_utils::unquote_path("'a b'"), "a b", test_name,
                      "single-quoted path should be unquoted")) {
        return false;
    }

    if (!expect_streq(completion_utils::unquote_path("a\\ b"), "a b", test_name,
                      "escaped whitespace should be unescaped")) {
        return false;
    }

    return true;
}

static bool test_quote_path_special_characters() {
    const char* test_name = "quote_path_special_characters";
    std::string path = "one&two";
    return expect_streq(completion_utils::quote_path_if_needed(path), "\"one&two\"", test_name,
                        "paths with special characters should be quoted");
}

static bool test_quote_path_empty_and_dollar() {
    const char* test_name = "quote_path_empty_and_dollar";
    if (!expect_streq(completion_utils::quote_path_if_needed(""), "", test_name,
                      "empty path should remain empty")) {
        return false;
    }

    std::string path = "cost$1";
    return expect_streq(completion_utils::quote_path_if_needed(path), "\"cost$1\"", test_name,
                        "paths containing shell metacharacters should be quoted");
}

static bool test_unquote_path_with_escaped_quote() {
    const char* test_name = "unquote_path_with_escaped_quote";
    return expect_streq(completion_utils::unquote_path("\"a\\\"b\""), "a\"b", test_name,
                        "escaped quotes should be unescaped");
}

static bool test_unquote_path_with_mixed_quote_segments() {
    const char* test_name = "unquote_path_with_mixed_quote_segments";
    return expect_streq(completion_utils::unquote_path("'left'\"right\"\\ middle"),
                        "leftright middle", test_name,
                        "single quotes, double quotes, and escaped spaces should combine");
}

static bool test_tokenize_command_line() {
    const char* test_name = "tokenize_command_line";
    std::string line = "cmd \"arg with space\" 'single quoted' plain\\ space";
    auto tokens = completion_utils::tokenize_command_line(line);

    EXPECT_TRUE(tokens.size() == 4, test_name, "expected four tokens");
    EXPECT_TRUE(tokens[0] == "cmd", test_name, "first token should be command");
    EXPECT_TRUE(tokens[1] == "arg with space", test_name,
                "double-quoted token should preserve spaces");
    EXPECT_TRUE(tokens[2] == "single quoted", test_name,
                "single-quoted token should preserve spaces");
    EXPECT_TRUE(tokens[3] == "plain space", test_name, "escaped space should join token");
    return true;
}

static bool test_tokenize_command_line_escaped_quotes() {
    const char* test_name = "tokenize_command_line_escaped_quotes";
    std::string line = "cmd \"a\\\"b\" tail";
    auto tokens = completion_utils::tokenize_command_line(line);

    EXPECT_TRUE(tokens.size() == 3, test_name, "expected three tokens");
    EXPECT_TRUE(tokens[0] == "cmd", test_name, "first token should be command");
    EXPECT_TRUE(tokens[1] == "a\"b", test_name, "escaped quote should be preserved in token");
    EXPECT_TRUE(tokens[2] == "tail", test_name, "last token should be preserved");
    return true;
}

static bool test_tokenize_command_line_unterminated_quote() {
    const char* test_name = "tokenize_command_line_unterminated_quote";
    std::string line = "cmd \"unterminated quote tail";
    auto tokens = completion_utils::tokenize_command_line(line);

    EXPECT_TRUE(tokens.size() == 2, test_name,
                "unterminated quoted text should remain a single token");
    EXPECT_TRUE(tokens[0] == "cmd", test_name, "first token should be command");
    EXPECT_TRUE(tokens[1] == "unterminated quote tail", test_name,
                "quoted payload should preserve spaces even when quote is unmatched");
    return true;
}

static bool test_completion_context_shell_state() {
    const char* test_name = "completion_context_shell_state";
    const std::string input =
        "printf 'left|right' | FOO=bar sudo -u root env -C /tmp BAR=\"x y\" command -- "
        "git -C repo checkout fe";
    auto context = completion_context::parse(input);

    const std::vector<std::string> expected = {"git", "-C", "repo", "checkout", "fe"};
    EXPECT_TRUE(context.effective_tokens == expected, test_name,
                "assignments and transparent wrappers should be removed from command state");
    EXPECT_TRUE(context.current_prefix == "fe", test_name,
                "the decoded token at the cursor should be retained");
    EXPECT_TRUE(context.current_raw_prefix == "fe", test_name,
                "the raw token at the cursor should be retained for replacement");
    EXPECT_FALSE(context.cursor_in_command_position, test_name,
                 "the cursor should be classified as an argument after the effective command");
    EXPECT_TRUE(context.segment_prefix.find("printf") == std::string::npos, test_name,
                "only the pipeline segment containing the cursor should remain active");
    return true;
}

static bool test_completion_context_cursor_and_quotes() {
    const char* test_name = "completion_context_cursor_and_quotes";
    const std::string input = "echo ignored | git checkout \"feature branch\" trailing";
    const std::size_t cursor = input.find(" trailing");
    auto context = completion_context::parse(input, cursor);

    const std::vector<std::string> expected = {"git", "checkout", "feature branch"};
    EXPECT_TRUE(context.effective_tokens == expected, test_name,
                "text after the cursor must not affect parsed command state");
    EXPECT_TRUE(context.current_prefix == "feature branch", test_name,
                "quoted current words should be decoded for semantic matching");
    EXPECT_TRUE(context.current_raw_prefix == "\"feature branch\"", test_name,
                "quoted current words should retain their raw replacement span");
    EXPECT_FALSE(context.at_word_boundary, test_name,
                 "a cursor touching a completed quoted word is still on that word");
    return true;
}

static bool test_completion_context_wrapper_value_state() {
    const char* test_name = "completion_context_wrapper_value_state";

    auto value_context = completion_context::parse("sudo -u ro");
    EXPECT_TRUE(value_context.effective_tokens.empty(), test_name,
                "a sudo option value must not be mistaken for a command");
    EXPECT_TRUE(value_context.cursor_in_wrapper_option_value, test_name,
                "wrapper option-value state should be exposed");
    EXPECT_FALSE(value_context.cursor_in_command_position, test_name,
                 "wrapper option values are argument positions");

    auto command_context = completion_context::parse("sudo -uroot ec");
    EXPECT_TRUE(command_context.effective_tokens == std::vector<std::string>({"ec"}), test_name,
                "attached wrapper option values should leave the next word as the command");
    EXPECT_TRUE(command_context.cursor_in_command_position, test_name,
                "the first effective word should be a command position");
    return true;
}

static bool test_completion_context_assignment_lhs_at_cursor() {
    const char* test_name = "completion_context_assignment_lhs_at_cursor";

    auto assignment_context = completion_context::parse("i=$((i+1))", 1);
    EXPECT_TRUE(assignment_context.cursor_in_assignment_lhs, test_name,
                "a cursor touching the equals sign should remain in the assignment name");

    const std::string partial_name = "VARIABLE=value";
    auto partial_context = completion_context::parse(partial_name, 3);
    EXPECT_TRUE(partial_context.cursor_in_assignment_lhs, test_name,
                "a cursor in the middle of an assignment name should be detected");

    const std::string indexed = "values[0]=2";
    auto indexed_context = completion_context::parse(indexed, indexed.find('='));
    EXPECT_TRUE(indexed_context.cursor_in_assignment_lhs, test_name,
                "an indexed assignment name should be detected");

    const std::string append = "items+=more";
    auto append_context = completion_context::parse(append, append.find('='));
    EXPECT_TRUE(append_context.cursor_in_assignment_lhs, test_name,
                "an append assignment name should be detected");

    auto value_context = completion_context::parse("i=$VALUE", 3);
    EXPECT_FALSE(value_context.cursor_in_assignment_lhs, test_name,
                 "completion should remain enabled after the assignment operator");

    auto quoted_context = completion_context::parse("\"i=value\"", 2);
    EXPECT_FALSE(quoted_context.cursor_in_assignment_lhs, test_name,
                 "quoted text containing equals must not be classified as an assignment");

    auto invalid_context = completion_context::parse("not-a-name=value", 10);
    EXPECT_FALSE(invalid_context.cursor_in_assignment_lhs, test_name,
                 "an invalid assignment name must not suppress normal completion");
    return true;
}

static bool test_completion_context_before_existing_word() {
    const char* test_name = "completion_context_before_existing_word";
    const std::string multiline = "while true; do\n    echo hello\ndone";
    const std::size_t done_start = multiline.rfind("done");

    auto before_context = completion_context::parse(multiline, done_start);
    EXPECT_TRUE(before_context.cursor_before_existing_word, test_name,
                "a cursor immediately before an existing word should be detected");

    auto inside_context = completion_context::parse("done", 2);
    EXPECT_FALSE(inside_context.cursor_before_existing_word, test_name,
                 "a cursor with a typed word prefix should still allow completion");

    auto end_context = completion_context::parse("done", 4);
    EXPECT_FALSE(end_context.cursor_before_existing_word, test_name,
                 "a cursor at the end of a word should still allow completion");

    auto separator_context = completion_context::parse("echo;done", 4);
    EXPECT_FALSE(separator_context.cursor_before_existing_word, test_name,
                 "a cursor before a control operator is not before an existing word");
    return true;
}

static bool test_tokenize_shell_words_preserve_literals() {
    const char* test_name = "tokenize_shell_words_preserve_literals";
    std::string line = "cmd \"arg with space\" plain\\ space 'single quoted'";
    auto tokens = command_line_utils::tokenize_shell_words(line, true);

    EXPECT_TRUE(tokens.size() == 4, test_name, "expected four tokens");
    EXPECT_TRUE(tokens[0] == "cmd", test_name, "first token should be command");
    EXPECT_TRUE(tokens[1] == "\"arg with space\"", test_name,
                "double quotes should be preserved when requested");
    EXPECT_TRUE(tokens[2] == "plain\\ space", test_name,
                "escaped space should retain the escape sequence");
    EXPECT_TRUE(tokens[3] == "'single quoted'", test_name,
                "single quotes should be preserved when requested");
    return true;
}

static bool test_history_completer_exit_code_ordering() {
    const char* test_name = "history_completer_exit_code_ordering";

    namespace fs = std::filesystem;
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temp_dir =
        fs::temp_directory_path() /
        ("cjsh_completion_history_test_" + std::to_string(static_cast<long long>(unique_suffix)));
    std::error_code ec;
    (void)fs::remove_all(temp_dir, ec);
    ec.clear();
    (void)fs::create_directories(temp_dir, ec);
    EXPECT_FALSE(ec, test_name, "temporary history directory should be created");

    std::ofstream history_file(cjsh_filesystem::g_cjsh_history_path());
    EXPECT_TRUE(history_file.is_open(), test_name, "temporary history file should open");
    history_file << "# code=127 time=1\n";
    history_file << "typo_current\n";
    history_file << "# exit_code=127 time=2\n";
    history_file << "typo_legacy\n";
    history_file << "# code=0 time=3\n";
    history_file << "type good\n";
    history_file << "# code=1 time=4\n";
    history_file << "type bad\n";
    history_file << "# code=0 time=5\n";
    history_file << "echo order_success\n";
    history_file << "# code=1 time=6\n";
    history_file << "echo order_failure\n";
    history_file << "# code=0 time=7\n";
    history_file << "echo order_directory\n";
    history_file.close();
    EXPECT_TRUE(history_file.good(), test_name, "temporary history file should be written");

    ssize_t count = run_completion_generation("ty", &cjsh_history_completer, 256);
    bool has_good = generated_completions_include_replacement("type good");
    bool has_bad = generated_completions_include_replacement("type bad");
    bool has_current_127 = generated_completions_include_replacement("typo_current");
    bool has_legacy_127 = generated_completions_include_replacement("typo_legacy");
    clear_generated_completions();

    std::ofstream completion_file(temp_dir / "order_file");
    EXPECT_TRUE(completion_file.is_open(), test_name, "temporary completion file should open");
    completion_file.close();
    (void)fs::create_directory(temp_dir / "order_directory", ec);
    EXPECT_FALSE(ec, test_name, "temporary completion directory should be created");

    const fs::path original_directory = fs::current_path(ec);
    EXPECT_FALSE(ec, test_name, "current directory should be readable");
    fs::current_path(temp_dir, ec);
    EXPECT_FALSE(ec, test_name, "temporary completion directory should be entered");

    (void)run_completion_generation("echo ord", &cjsh_default_completer, 256);
    const ssize_t successful_history_index = generated_completion_index_with_source("history: 0");
    const ssize_t file_index = generated_completion_index_with_source("file");
    const ssize_t failed_history_index = generated_completion_index_with_source("history: 1");
    const bool has_duplicate_history =
        generated_completions_include_replacement("echo order_directory");
    const bool has_directory_completion =
        generated_completions_include_replacement("order_directory/");
    clear_generated_completions();

    fs::current_path(original_directory, ec);
    EXPECT_FALSE(ec, test_name, "original completion directory should be restored");

    (void)fs::remove_all(temp_dir, ec);

    EXPECT_TRUE(count == 2, test_name, "only non-127 history entries should be offered");
    EXPECT_TRUE(has_good, test_name, "successful history entry should remain available");
    EXPECT_TRUE(has_bad, test_name, "failed history entry should remain available");
    EXPECT_FALSE(has_current_127, test_name, "code=127 history entry should be hidden");
    EXPECT_FALSE(has_legacy_127, test_name, "exit_code=127 history entry should be hidden");
    EXPECT_TRUE(successful_history_index >= 0, test_name,
                "successful history completion should be generated");
    EXPECT_TRUE(file_index >= 0, test_name, "file completion should be generated");
    EXPECT_TRUE(failed_history_index >= 0, test_name,
                "failed history completion should be generated");
    EXPECT_FALSE(has_duplicate_history, test_name,
                 "history completion duplicated by a directory should be omitted");
    EXPECT_TRUE(has_directory_completion, test_name,
                "the preferred directory completion should remain available");
    EXPECT_TRUE(successful_history_index < file_index, test_name,
                "successful history completion should precede file completions");
    EXPECT_TRUE(file_index < failed_history_index, test_name,
                "failed history completion should follow file completions");
    return true;
}

static bool test_history_directory_completions() {
    const char* test_name = "history_directory_completions";
    EXPECT_TRUE(
        write_completion_history("# cwd=%2Fproject%20space%2F%25work code=0\necho parent\n"
                                 "# cwd=%2Fproject%20space%2F%25work%2Fsrc code=0\necho child\n"
                                 "# cwd=%2Fproject%20space%2F%25work-other code=0\necho sibling\n"
                                 "# code=0\necho legacy\n"),
        test_name, "history fixture should be written");
    (void)ic_set_history_directory("/project space/%work");
    const bool previous_scope = ic_enable_history_directory(true);
    const bool previous_subdirs = ic_enable_history_directory_subdirs(false);
    (void)run_completion_generation("echo", &cjsh_history_completer, 256);
    const auto exact = generated_completion_replacements();
    clear_generated_completions();
    (void)ic_enable_history_directory_subdirs(true);
    (void)run_completion_generation("", &cjsh_default_completer, 256);
    const auto nested = generated_completion_replacements();
    clear_generated_completions();
    (void)ic_enable_history_directory(false);
    const auto global_count = run_completion_generation("echo", &cjsh_history_completer, 256);
    clear_generated_completions();
    (void)ic_enable_history_directory(previous_scope);
    (void)ic_enable_history_directory_subdirs(previous_subdirs);
    (void)ic_set_history_directory(nullptr);
    EXPECT_TRUE(exact == std::vector<std::string>{"echo parent"}, test_name,
                "prefix completions must decode directory metadata and scope before matching");
    const std::vector<std::string> expected_nested = {"echo child", "echo parent"};
    EXPECT_TRUE(nested == expected_nested, test_name,
                "empty-prompt suggestions include descendants but exclude siblings and legacy");
    EXPECT_TRUE(global_count == 4, test_name,
                "disabling scope restores every directory and legacy");
    return true;
}

static bool test_history_prefix_metadata_isolation() {
    const char* test_name = "history_prefix_metadata_isolation";
    EXPECT_TRUE(write_completion_history("# code=127\nunmatched command\n"
                                         "audit after_unmatched\n"
                                         "# code=127\naudit hidden\n"
                                         "audit after_hidden\n"
                                         "# code=127\n# code=0\n\naudit last_header\n"
                                         "# code=127\n#\naudit cleared_header\n"
                                         "# code=127\naudit\n"
                                         "audit after_exact_prefix\n"
                                         "# code=0\naudit escaped\\ncontinuation\n"),
                test_name, "history fixture should be written");
    const ssize_t count = run_completion_generation("audit", &cjsh_history_completer, 256);
    const bool ok = count == 6 &&
                    generated_completions_include_replacement("audit after_unmatched") &&
                    generated_completions_include_replacement("audit after_hidden") &&
                    generated_completions_include_replacement("audit last_header") &&
                    generated_completions_include_replacement("audit cleared_header") &&
                    generated_completions_include_replacement("audit after_exact_prefix") &&
                    generated_completions_include_replacement("audit escaped\ncontinuation") &&
                    !generated_completions_include_replacement("audit hidden");
    clear_generated_completions();
    EXPECT_TRUE(ok, test_name,
                "metadata must apply only to the next command, including skipped entries");
    EXPECT_TRUE(run_completion_generation("absent_prefix", &cjsh_history_completer, 256) == 0,
                test_name, "unsuccessful prefix lookup should return no history entries");
    clear_generated_completions();
    return true;
}

static bool test_empty_prompt_history_ranking() {
    const char* test_name = "empty_prompt_history_ranking";
    EXPECT_TRUE(
        write_completion_history("# timestamp=100 frequency=900 code=0\necho old\n"
                                 "# timestamp=400 frequency=1 code=1\ncat ./recent.txt\n"
                                 "# timestamp=300 frequency=2 code=0\necho lower_frequency\n"
                                 "# timestamp=300 frequency=20 code=0\necho frequent\n"
                                 "# timestamp=300 frequency=20 code=0\necho tied_later\n"
                                 "# timestamp=350 frequency=901 code=0\necho old\n"
                                 "# timestamp=500 frequency=1 code=127\nnotfound\n"),
        test_name, "history fixture should be written");

    const std::vector<std::string> expected = {"cat ./recent.txt", "echo old", "echo tied_later",
                                               "echo frequent", "echo lower_frequency"};
    for (const char* input : {"", " \t", "\n  "}) {
        const ssize_t count = run_completion_generation(input, &cjsh_default_completer, 256);
        EXPECT_TRUE(count == 5, test_name,
                    "blank prompts should offer only unique eligible history entries");
        EXPECT_TRUE(generated_completion_replacements() == expected, test_name,
                    "recency should outrank frequency, with later records breaking exact ties");
        EXPECT_TRUE(first_generated_completion_matches("cat ./recent.txt", "history: 1"), test_name,
                    "paths and nonzero exit codes should not override usage ranking");
        clear_generated_completions();
    }

    (void)run_completion_generation("echo o", &cjsh_default_completer, 256);
    EXPECT_TRUE(first_generated_completion_matches("echo old", "history: 0"), test_name,
                "typed prefixes should retain their usual history completions");
    clear_generated_completions();
    return true;
}

static bool test_empty_prompt_history_limits() {
    const char* test_name = "empty_prompt_history_limits";
    std::string history;
    for (int i = 0; i < 60; ++i) {
        history += "# timestamp=" + std::to_string(i + 1) + " frequency=1 code=0\necho entry_" +
                   std::to_string(i) + "\n";
    }
    for (int i = 0; i < 20; ++i) {
        history +=
            "# timestamp=" + std::to_string(i + 100) + " frequency=1 code=0\necho repeated\n";
    }
    history += "# timestamp=1000 frequency=1 code=0\necho newest\n";
    EXPECT_TRUE(write_completion_history(history), test_name, "history fixture should be written");

    std::vector<std::string> expected = {"echo newest", "echo repeated"};
    for (int i = 59; i >= 0; --i) {
        expected.push_back("echo entry_" + std::to_string(i));
    }
    const long previous_limit = get_completion_max_results();
    for (long limit : {1L, 2L, 25L, 55L, get_completion_default_max_results()}) {
        const size_t expected_count = std::min(expected.size(), static_cast<size_t>(limit));
        const std::vector<std::string> limited_expected(expected.begin(),
                                                        expected.begin() + expected_count);
        for (const char* input : {"", " \t"}) {
            EXPECT_TRUE(set_completion_max_results(limit), test_name,
                        "completion cap should be accepted");
            const ssize_t count = run_completion_generation(input, &cjsh_default_completer, 256);
            const auto replacements = generated_completion_replacements();
            (void)set_completion_max_results(previous_limit);
            clear_generated_completions();
            EXPECT_TRUE(count == static_cast<ssize_t>(expected_count), test_name,
                        "blank prompts should use the configured cap, including values above 15");
            EXPECT_TRUE(replacements == limited_expected, test_name,
                        "the configured cap must apply after ranking and deduplication of all "
                        "history entries, including those beyond the first 50 matches");
        }
    }
    return true;
}

static bool test_empty_prompt_legacy_history() {
    const char* test_name = "empty_prompt_legacy_history";
    EXPECT_TRUE(write_completion_history(
                    "echo legacy_old\n"
                    "# timestamp=broken frequency=-1\necho malformed\n"
                    "# timestamp=999999999999999999999999 frequency=999999999999999999999999\n"
                    "echo overflow\n"
                    "# timestamp=100 frequency=1 code=0\necho dated\n"
                    "echo legacy_new\n"
                    "# timestamp=200 frequency=1 code=127\nnotfound\n"
                    "echo after_hidden\n"
                    "# timestamp=300 frequency=1 code=0\n   \n"
                    "echo first\\necho second\n"),
                test_name, "legacy history fixture should be written");
    (void)run_completion_generation("", &cjsh_default_completer, 256);
    const std::vector<std::string> expected = {
        "echo dated",    "echo first\necho second", "echo after_hidden", "echo legacy_new",
        "echo overflow", "echo malformed",          "echo legacy_old"};
    EXPECT_TRUE(generated_completion_replacements() == expected, test_name,
                "undated records should follow dated records in reverse file order; invalid "
                "metadata must not leak to following records");
    clear_generated_completions();
    return true;
}

static bool test_history_metadata_field_boundaries() {
    const char* test_name = "history_metadata_field_boundaries";
    EXPECT_TRUE(write_completion_history(
                    "#\tcode=127\vexit_code=+0\ftimestamp=20\tfrequency=2\necho newest\n"
                    "#code=+127 timestamp=99\necho hidden\n"
                    "# code=-1 timestamp=10 frequency=3\necho negative_status\n"
                    "# code=127x timestamp=9x frequency=1x\necho invalid_fields\n"
                    "# code=0 code=127 exit_code=oops timestamp=99\necho hidden_duplicate\n"
                    "# =bad code= timestamp=+100 frequency=0\necho default_fields\n"
                    "# timestamp=10 frequency=5 ignored=a=b\necho frequent\n"
                    "#\necho bare_header\n"),
                test_name, "history fixture should be written");
    (void)run_completion_generation("", &cjsh_default_completer, 256);
    const std::vector<std::string> expected{"echo newest",          "echo frequent",
                                            "echo negative_status", "echo bare_header",
                                            "echo default_fields",  "echo invalid_fields"};
    const bool ok = generated_completion_replacements() == expected &&
                    first_generated_completion_matches("echo newest", "history: 0");
    clear_generated_completions();
    EXPECT_TRUE(ok, test_name,
                "metadata fields must retain signs, delimiters, duplicate precedence and defaults");
    return true;
}

static bool test_empty_prompt_without_history() {
    const char* test_name = "empty_prompt_without_history";
    EXPECT_TRUE(write_completion_history("# timestamp=100 frequency=1 code=0\necho saved\n"),
                test_name, "history fixture should be written");
    const bool history_enabled = config::history_enabled;
    config::history_enabled = false;
    const ssize_t disabled_count = run_completion_generation("", &cjsh_default_completer, 256);
    config::history_enabled = history_enabled;
    clear_generated_completions();
    EXPECT_TRUE(disabled_count == 0, test_name, "disabled history should leave empty Tab empty");

    EXPECT_TRUE(write_completion_history(""), test_name, "history fixture should be cleared");
    EXPECT_TRUE(run_completion_generation("", &cjsh_default_completer, 256) == 0, test_name,
                "empty history should not fall back to files or PATH commands");
    std::error_code ec;
    (void)std::filesystem::remove(cjsh_filesystem::g_cjsh_history_path(), ec);
    EXPECT_FALSE(ec, test_name, "history fixture should be removed");
    EXPECT_TRUE(run_completion_generation(" \t", &cjsh_default_completer, 256) == 0, test_name,
                "missing history should not fall back to files or PATH commands");
    clear_generated_completions();
    return true;
}

static bool test_default_completer_command_in_command_substitution() {
    const char* test_name = "default_completer_command_in_command_substitution";
    ssize_t count = run_completion_generation("$(ech", &cjsh_default_completer, 256);

    EXPECT_TRUE(count > 0, test_name,
                "default completer should return suggestions for command substitution prefix");
    bool has_command = generated_completions_include_replacement("echo ");
    clear_generated_completions();

    EXPECT_TRUE(has_command, test_name,
                "command substitution scope should include command completions");
    return true;
}

static bool test_default_completer_nested_command_substitution_scope() {
    const char* test_name = "default_completer_nested_command_substitution_scope";
    ssize_t count =
        run_completion_generation("echo $(printf '%s' $(ech", &cjsh_default_completer, 256);

    EXPECT_TRUE(count > 0, test_name,
                "default completer should return suggestions inside nested command substitution");
    bool has_command = generated_completions_include_replacement("echo ");
    clear_generated_completions();

    EXPECT_TRUE(has_command, test_name,
                "innermost command substitution should drive command completions");
    return true;
}

static bool test_default_completer_split_unknown_command_merge() {
    const char* test_name = "default_completer_split_unknown_command_merge";
    ssize_t count = run_completion_generation("pri ntf", &cjsh_default_completer, 256);
    bool has_command = generated_completions_include_replacement("printf ") ||
                       generated_completions_include_replacement("printf");
    clear_generated_completions();

    EXPECT_TRUE(count > 0, test_name,
                "default completer should return exact split-command suggestions");
    EXPECT_TRUE(has_command, test_name,
                "exact split command tokens should still merge into known completions");
    return true;
}

static bool test_default_completer_spell_follows_command_cursor() {
    const char* test_name = "default_completer_spell_follows_command_cursor";
    const bool original_spell_setting = is_completion_spell_correction_enabled();
    set_completion_spell_correction_enabled(true);

    ssize_t command_count = run_completion_generation_at("sl add", 2, &cjsh_default_completer, 256);
    bool has_command_correction = generated_completions_include_replacement("ls");
    bool correction_is_first = first_generated_completion_matches("ls", "spell");
    clear_generated_completions();

    (void)run_completion_generation("sl add", &cjsh_default_completer, 256);
    bool has_argument_correction = generated_completions_include_replacement("ls");
    clear_generated_completions();

    set_completion_spell_correction_enabled(original_spell_setting);

    EXPECT_TRUE(command_count > 0, test_name,
                "the command token should offer spell correction when the cursor touches it");
    EXPECT_TRUE(has_command_correction, test_name,
                "the misspelled command should include its spell correction");
    EXPECT_TRUE(correction_is_first, test_name,
                "an adjacent-transposition correction should precede prefix completions");
    EXPECT_FALSE(has_argument_correction, test_name,
                 "an argument cursor must not offer a correction for the command token");
    return true;
}

static bool test_default_completer_does_not_spell_correct_assignments() {
    const char* test_name = "default_completer_does_not_spell_correct_assignments";
    const bool original_spell_setting = is_completion_spell_correction_enabled();
    set_completion_spell_correction_enabled(true);

    (void)run_completion_generation("MAX=10000\nprimes=\"\"\ni=2", &cjsh_default_completer, 256);
    bool multiline_has_spell_correction = generated_completions_include_source("spell");
    clear_generated_completions();

    (void)run_completion_generation("values[0]=2", &cjsh_default_completer, 256);
    bool array_has_spell_correction = generated_completions_include_source("spell");
    clear_generated_completions();

    set_completion_spell_correction_enabled(original_spell_setting);

    EXPECT_FALSE(multiline_has_spell_correction, test_name,
                 "a multiline assignment must not be treated as a misspelled command");
    EXPECT_FALSE(array_has_spell_correction, test_name,
                 "an array assignment must not be treated as a misspelled command");
    return true;
}

static bool test_default_completer_suppresses_assignment_lhs_midline() {
    const char* test_name = "default_completer_suppresses_assignment_lhs_midline";
    const std::string input =
        "MAX=1000000\n"
        "primes=\"\"\n"
        "i=2\n"
        "while [ $i -le $MAX ]; do\n"
        "    is_prime=1\n"
        "    i=$((i+1))\n"
        "done";
    const std::size_t assignment = input.find("i=$((i+1))");
    EXPECT_TRUE(assignment != std::string::npos, test_name,
                "the regression fixture should contain the assignment");

    const ssize_t cursor = static_cast<ssize_t>(assignment + 1);
    ssize_t count =
        run_completion_generation_at(input.c_str(), cursor, &cjsh_default_completer, 256);
    bool has_control_structure = generated_completions_include_source("control structure");
    clear_generated_completions();

    EXPECT_TRUE(count == 0, test_name,
                "an assignment name before an existing equals sign should offer no completions");
    EXPECT_FALSE(has_control_structure, test_name,
                 "the assignment variable i must not offer the multiline if completion");

    (void)run_completion_generation("i=$", &cjsh_default_completer, 256);
    bool has_value_completion = generated_completions_include_source("variable");
    clear_generated_completions();
    EXPECT_TRUE(has_value_completion, test_name,
                "variable completion should remain available in the assignment value");
    return true;
}

static bool test_default_completer_suppresses_before_existing_word() {
    const char* test_name = "default_completer_suppresses_before_existing_word";
    const std::string input =
        "MAX=1000000\n"
        "primes=\"\"\n"
        "i=2\n"
        "while [ $i -le $MAX ]; do\n"
        "    echo $i\n"
        "done";
    const std::size_t done_start = input.rfind("done");
    EXPECT_TRUE(done_start != std::string::npos, test_name,
                "the regression fixture should contain the closing done");

    ssize_t count = run_completion_generation_at(input.c_str(), static_cast<ssize_t>(done_start),
                                                 &cjsh_default_completer, 256);
    clear_generated_completions();
    EXPECT_TRUE(count == 0, test_name,
                "the cursor before done should not offer an insertion completion");

    (void)run_completion_generation("do", &cjsh_default_completer, 256);
    bool inside_word_completion = generated_completions_include_replacement("done ");
    clear_generated_completions();
    EXPECT_TRUE(inside_word_completion, test_name,
                "completion should remain available for an unfinished word");
    return true;
}

static bool test_default_completer_suppresses_inside_known_command() {
    const char* test_name = "default_completer_suppresses_inside_known_command";
    const std::string input =
        "MAX=1000000\n"
        "primes=\"\"\n"
        "i=2\n"
        "while [ $i -le $MAX ]; do\n"
        "    is_prime=1\n"
        "    for p in $primes; do\n"
        "        if [ $((p*p)) -gt $i ]; then\n"
        "            break\n"
        "        fi\n"
        "        if [ $((i % p)) -eq 0 ]; then\n"
        "            is_prime=0\n"
        "            break\n"
        "        fi\n"
        "    done\n"
        "    if [ $is_prime -eq 1 ]; then\n"
        "        echo $i\n"
        "        primes=\"$primes $i\"\n"
        "    fi\n"
        "    i=$((i+1))\n"
        "done";
    const std::size_t then_start = input.find("then", input.find("i % p"));
    EXPECT_TRUE(then_start != std::string::npos, test_name,
                "the regression fixture should contain then on line ten");
    EXPECT_TRUE(write_completion_history("# code=0 time=1\ntests/\n"), test_name,
                "a competing completion should be available for the t prefix");

    // Inline hints use a smaller result budget than the completion menu.
    for (ssize_t limit : {2, 256}) {
        for (std::size_t offset : {1, 2, 3}) {
            const ssize_t count = run_completion_generation_at(
                input.c_str(), static_cast<ssize_t>(then_start + offset), &cjsh_default_completer,
                limit);
            clear_generated_completions();
            EXPECT_TRUE(count == 0, test_name,
                        "a cursor inside the existing then must not offer completions");
        }
    }

    const std::vector<std::pair<std::string, ssize_t>> known_commands = {
        {"done", 2},
        {"echo argument", 2},
        {"echo;next", 2},
        {"echo>output", 2},
        {"$(echo)", 4},
        {"sudo echo argument", 7},
        {"\"echo\" argument", 3},
        {"e'cho' argument", 2},
        {"ec\\ho argument", 2},
    };
    for (const auto& entry : known_commands) {
        const ssize_t count = run_completion_generation_at(entry.first.c_str(), entry.second,
                                                           &cjsh_default_completer, 256);
        clear_generated_completions();
        EXPECT_TRUE(count == 0, test_name,
                    "a recognized command or keyword should not be completed from its prefix");
    }
    return true;
}

static bool test_known_shell_command_completion_without_path() {
    const char* test_name = "known_shell_command_completion_without_path";
    ScopedEnvironmentValue path("PATH", "/cjsh-completion-nonexistent-path");
    auto previous_shell = std::move(g_shell);
    const bool previous_interactive = config::interactive_mode;
    config::interactive_mode = false;
    g_shell = std::make_unique<Shell>();
    const bool ok = [&] {
        g_shell->set_aliases({{"auditalias", "echo"}});
        EXPECT_TRUE(g_shell->execute("auditfunction() { :; }") == 0, test_name,
                    "function fixture should be defined");
        EXPECT_TRUE(write_completion_history("echo_more\nauditalias_more\nauditfunction_more\n"),
                    test_name, "competing history completions should exist");
        for (const char* command :
             {"echo argument", "auditalias argument", "auditfunction argument"}) {
            const auto count =
                run_completion_generation_at(command, 2, &cjsh_default_completer, 256);
            clear_generated_completions();
            EXPECT_TRUE(count == 0, test_name,
                        "builtins, aliases and functions stay known without any PATH entries");
        }
        g_shell->set_aliases({});
        {
            const auto count = run_completion_generation_at("auditalias argument", 2,
                                                            &cjsh_default_completer, 256);
            clear_generated_completions();
            EXPECT_TRUE(count > 0, test_name, "removed aliases must become unknown immediately");
        }
        g_shell = std::make_unique<Shell>();
        for (const char* command : {"auditalias argument", "auditfunction argument"}) {
            const auto count =
                run_completion_generation_at(command, 2, &cjsh_default_completer, 256);
            clear_generated_completions();
            EXPECT_TRUE(count > 0, test_name,
                        "a new shell must not retain old alias or function classifications");
        }
        return true;
    }();
    g_shell = std::move(previous_shell);
    config::interactive_mode = previous_interactive;
    return ok;
}

static bool test_default_completer_keeps_unfinished_command_completions() {
    const char* test_name = "default_completer_keeps_unfinished_command_completions";
    const std::vector<std::pair<std::string, ssize_t>> unfinished_commands = {
        {"ech", 2},
        {"ech", 3},
        {"ech argument", 3},
        {"echoes_not_a_command", 2},
        {"\"echo argument\"", 3},
        {"echo\\ argument", 2},
        {"\"echo", 3},
    };
    for (const auto& entry : unfinished_commands) {
        (void)run_completion_generation_at(entry.first.c_str(), entry.second,
                                           &cjsh_default_completer, 256);
        const bool has_echo = generated_completions_include_replacement("echo ");
        clear_generated_completions();
        EXPECT_TRUE(has_echo, test_name,
                    "completion should remain available inside unknown words and at word ends");
    }
    return true;
}

static bool test_find_last_unquoted_space() {
    const char* test_name = "find_last_unquoted_space";
    std::string line = "echo \"a b\" c";
    size_t pos = completion_utils::find_last_unquoted_space(line);
    EXPECT_TRUE(pos == 10, test_name, "last unquoted space should be before final token");
    EXPECT_TRUE(completion_utils::find_last_unquoted_space("\"a b\"") == std::string::npos,
                test_name, "quoted spaces should be ignored");
    return true;
}

static bool test_find_last_unquoted_space_with_tabs() {
    const char* test_name = "find_last_unquoted_space_with_tabs";
    std::string line = "cmd\targ";
    size_t pos = completion_utils::find_last_unquoted_space(line);
    EXPECT_TRUE(pos == 3, test_name, "tab should count as a delimiter");
    return true;
}

static bool test_find_last_unquoted_space_with_escaped_space() {
    const char* test_name = "find_last_unquoted_space_with_escaped_space";
    std::string escaped_only = "escaped\\ space";
    EXPECT_TRUE(completion_utils::find_last_unquoted_space(escaped_only) == std::string::npos,
                test_name, "escaped whitespace should not be treated as a delimiter");

    std::string with_tail = "escaped\\ space tail";
    size_t expected = with_tail.find(" tail");
    EXPECT_TRUE(expected != std::string::npos, test_name, "expected reference delimiter");
    size_t pos = completion_utils::find_last_unquoted_space(with_tail);
    EXPECT_TRUE(pos == expected, test_name,
                "last unquoted delimiter should ignore escaped whitespace");
    return true;
}

static bool test_find_last_unquoted_space_with_unterminated_quote() {
    const char* test_name = "find_last_unquoted_space_with_unterminated_quote";
    std::string line = "cmd \"quoted words remain quoted";
    size_t pos = completion_utils::find_last_unquoted_space(line);
    EXPECT_TRUE(pos == 3, test_name,
                "spaces inside an unterminated quote should not count as delimiters");
    return true;
}

static bool test_case_sensitivity_helpers() {
    const char* test_name = "case_sensitivity_helpers";
    const bool original_setting = is_completion_case_sensitive();
    set_completion_case_sensitive(false);
    EXPECT_TRUE(completion_utils::matches_completion_prefix("Hello", "he"), test_name,
                "case-insensitive prefix should match");
    EXPECT_TRUE(completion_utils::equals_completion_token("FOO", "foo"), test_name,
                "case-insensitive token should match");

    set_completion_case_sensitive(true);
    EXPECT_FALSE(completion_utils::matches_completion_prefix("Hello", "he"), test_name,
                 "case-sensitive prefix should reject mismatch");
    EXPECT_FALSE(completion_utils::equals_completion_token("FOO", "foo"), test_name,
                 "case-sensitive token should reject mismatch");

    set_completion_case_sensitive(original_setting);
    return true;
}

static bool test_normalize_for_comparison() {
    const char* test_name = "normalize_for_comparison";
    const bool original_setting = is_completion_case_sensitive();
    set_completion_case_sensitive(false);
    if (!expect_streq(completion_utils::normalize_for_comparison("MiXeD"), "mixed", test_name,
                      "normalize should lower-case when case-insensitive")) {
        set_completion_case_sensitive(original_setting);
        return false;
    }

    set_completion_case_sensitive(true);
    if (!expect_streq(completion_utils::normalize_for_comparison("MiXeD"), "MiXeD", test_name,
                      "normalize should preserve case when case-sensitive")) {
        set_completion_case_sensitive(original_setting);
        return false;
    }

    set_completion_case_sensitive(original_setting);
    return true;
}

static bool test_starts_with_helpers() {
    const char* test_name = "starts_with_helpers";
    EXPECT_TRUE(completion_utils::starts_with_case_insensitive("Hello", "he"), test_name,
                "case-insensitive helper should match");
    EXPECT_TRUE(completion_utils::starts_with_case_insensitive("Hello", "HE"), test_name,
                "case-insensitive helper should match uppercase prefix");
    EXPECT_FALSE(completion_utils::starts_with_case_insensitive("Hello", "hi"), test_name,
                 "case-insensitive helper should reject mismatched prefix");

    EXPECT_TRUE(completion_utils::starts_with_case_sensitive("Hello", "He"), test_name,
                "case-sensitive helper should match exact prefix");
    EXPECT_FALSE(completion_utils::starts_with_case_sensitive("Hello", "he"), test_name,
                 "case-sensitive helper should reject mismatched case");
    EXPECT_FALSE(completion_utils::starts_with_case_sensitive("Hello", "HelloWorld"), test_name,
                 "case-sensitive helper should reject longer prefix");
    return true;
}

static bool test_sanitize_job_summary() {
    const char* test_name = "sanitize_job_summary";
    std::string raw = "  ls\t -la \n \x01";
    return expect_streq(completion_utils::sanitize_job_command_summary(raw), "ls -la", test_name,
                        "summary should normalize whitespace and strip control chars");
}

static bool test_sanitize_job_summary_truncates() {
    const char* test_name = "sanitize_job_summary_truncates";
    std::string long_cmd(120, 'a');
    std::string summary = completion_utils::sanitize_job_command_summary(long_cmd);
    EXPECT_TRUE(summary.size() == 80, test_name, "summary should truncate to 80 characters");
    return true;
}

static bool test_sanitize_job_summary_whitespace_only() {
    const char* test_name = "sanitize_job_summary_whitespace_only";
    std::string raw = " \t \n \r\x01\x02";
    return expect_streq(completion_utils::sanitize_job_command_summary(raw), "", test_name,
                        "all whitespace/control input should sanitize to empty string");
}

static bool test_spell_transposition_and_distance() {
    const char* test_name = "spell_transposition_and_distance";
    EXPECT_TRUE(completion_spell::is_adjacent_transposition("abcd", "abdc"), test_name,
                "adjacent transposition should be detected");
    EXPECT_FALSE(completion_spell::is_adjacent_transposition("abcd", "adbc"), test_name,
                 "non-adjacent swap should not be treated as transposition");

    EXPECT_TRUE(completion_spell::compute_edit_distance_with_limit("kitten", "sitting", 3) == 3,
                test_name, "edit distance should match expected value");
    EXPECT_TRUE(completion_spell::compute_edit_distance_with_limit("kitten", "sitting", 2) == 3,
                test_name, "edit distance should exceed limit and return max+1");

    EXPECT_FALSE(completion_spell::should_consider_spell_correction("a"), test_name,
                 "single-character prefix should not trigger spell correction");
    EXPECT_TRUE(completion_spell::should_consider_spell_correction("ab"), test_name,
                "two-character prefix should trigger spell correction");
    return true;
}

static bool test_spell_distance_negative_limit() {
    const char* test_name = "spell_distance_negative_limit";
    int distance = completion_spell::compute_edit_distance_with_limit("abc", "xyz", -1);
    EXPECT_TRUE(distance == std::numeric_limits<int>::max(), test_name,
                "negative max distance should return sentinel max value");
    return true;
}

static bool test_spell_match_ordering() {
    const char* test_name = "spell_match_ordering";
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> matches;
    matches["alpha"] = {"alpha", 2, false, 2};
    matches["alhpa"] = {"alhpa", 1, true, 1};
    matches["alpah"] = {"alpah", 1, false, 3};
    matches["alps"] = {"alps", 1, false, 2};

    auto ordered = completion_spell::order_spell_correction_matches(matches);
    EXPECT_TRUE(ordered.size() == 4, test_name, "expected four spell matches");
    EXPECT_TRUE(ordered[0].candidate == "alhpa", test_name,
                "transposition match should rank ahead of other distance-1 matches");
    EXPECT_TRUE(ordered[1].candidate == "alpah", test_name,
                "shared prefix length should break distance ties");
    EXPECT_TRUE(ordered[2].candidate == "alps", test_name,
                "remaining distance-1 match should follow by shared prefix length");
    EXPECT_TRUE(ordered[3].candidate == "alpha", test_name,
                "higher distance match should rank last");
    return true;
}

static bool test_spell_match_add_limit() {
    const char* test_name = "spell_match_add_limit";
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> matches;
    for (int i = 0; i < 20; ++i) {
        std::string name = "spell" + std::to_string(i);
        matches[name] = {name, 1, false, 1};
    }

    g_spell_matches = &matches;
    g_spell_prefix_len = 4;
    ssize_t count = run_completion_generation("spel", &spell_match_completer, 64);
    g_spell_matches = nullptr;
    g_spell_prefix_len = 0;

    EXPECT_TRUE(count == 10, test_name, "spell match insertion should cap at 10 entries");
    return true;
}

static bool test_collect_spell_candidates_filter_and_case_normalization() {
    const char* test_name = "collect_spell_candidates_filter_and_case_normalization";
    const bool original_setting = is_completion_case_sensitive();
    set_completion_case_sensitive(false);

    std::vector<std::string> candidates = {"Git", "gti", "grep"};
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> matches;
    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; },
        [](const std::string& value) { return value != "grep"; }, "gti", matches);

    bool ok = true;
    if (matches.size() != 1) {
        log_failure(test_name, "filtering and exact-match skipping should leave one candidate");
        ok = false;
    }

    auto it = matches.find("Git");
    if (it == matches.end()) {
        log_failure(test_name, "case-insensitive normalized transposition should be collected");
        ok = false;
    } else {
        if (it->second.distance != 1) {
            log_failure(test_name, "transposition should use effective distance of 1");
            ok = false;
        }
        if (!it->second.is_transposition) {
            log_failure(test_name, "candidate should be marked as transposition");
            ok = false;
        }
        if (it->second.shared_prefix_len != 1) {
            log_failure(test_name, "shared prefix length should be computed on normalized text");
            ok = false;
        }
    }

    set_completion_case_sensitive(original_setting);
    return ok;
}

static bool test_collect_spell_candidates_distance_thresholds() {
    const char* test_name = "collect_spell_candidates_distance_thresholds";
    std::vector<std::string> candidates = {"abcxxx", "abxxxx", "abxyz"};

    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> long_prefix_matches;
    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; },
        [](const std::string&) { return true; }, "abcdef", long_prefix_matches);

    EXPECT_TRUE(long_prefix_matches.find("abcxxx") != long_prefix_matches.end(), test_name,
                "distance-3 candidate should be kept for longer prefixes");
    EXPECT_TRUE(long_prefix_matches.find("abxxxx") == long_prefix_matches.end(), test_name,
                "distance-4 candidate should be discarded for longer prefixes");

    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> short_prefix_matches;
    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; },
        [](const std::string&) { return true; }, "abcde", short_prefix_matches);

    EXPECT_TRUE(short_prefix_matches.find("abxyz") == short_prefix_matches.end(), test_name,
                "distance-3 candidate should be discarded for short prefixes");
    return true;
}

static bool test_collect_spell_candidates_without_filter() {
    const char* test_name = "collect_spell_candidates_without_filter";
    std::vector<std::string> candidates = {"gti"};
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> matches;
    const std::function<bool(const std::string&)> no_filter;

    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; }, no_filter, "git", matches);

    EXPECT_TRUE(matches.find("gti") != matches.end(), test_name,
                "empty filter should behave as allow-all");
    return true;
}

static bool test_collect_transpositions_only() {
    const char* test_name = "collect_transpositions_only";
    const bool original_setting = is_completion_case_sensitive();
    set_completion_case_sensitive(false);
    const std::vector<std::string> candidates{"Git", "gti", "got", "grit", "unrelated"};
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> transpositions;
    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> all;
    std::vector<std::string> inspected;
    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; },
        [&](const std::string& candidate) {
            inspected.push_back(candidate);
            return true;
        },
        "gti", transpositions, true);
    completion_spell::collect_spell_correction_candidates(
        candidates, [](const std::string& value) { return value; }, {}, "gti", all);
    set_completion_case_sensitive(original_setting);
    EXPECT_TRUE(inspected == std::vector<std::string>{"Git"}, test_name,
                "only adjacent transpositions should reach the candidate filter");
    EXPECT_TRUE(transpositions.size() == 1 && transpositions.count("Git") == 1, test_name,
                "exact matches and ordinary edits are excluded from the first pass");
    const auto& match = transpositions.at("Git");
    EXPECT_TRUE(match.is_transposition && match.distance == 1 && match.shared_prefix_len == 1,
                test_name, "normalized transpositions retain their ranking");
    for (const auto& [candidate, result] : all) {
        EXPECT_TRUE((transpositions.count(candidate) != 0) == result.is_transposition, test_name,
                    "the first pass agrees with filtering the full candidate set");
    }
    return true;
}

static bool test_completion_tracker_deduplication() {
    const char* test_name = "completion_tracker_deduplication";
    g_completion_actions = {
        {"d", 1, 0, "test"},
        {"bd", 2, 0, "test"},
    };
    ssize_t count = run_completion_generation("abc", &completion_action_completer, 64);
    g_completion_actions.clear();

    EXPECT_TRUE(count == 1, test_name, "duplicate final result should only be added once");
    return true;
}

static bool test_completion_tracker_trims_trailing_spaces() {
    const char* test_name = "completion_tracker_trims_trailing_spaces";
    g_completion_actions = {
        {"arg ", 0, 0, "test"},
        {"arg", 0, 0, "test"},
    };
    ssize_t count = run_completion_generation("cmd ", &completion_action_completer, 64);
    g_completion_actions.clear();

    EXPECT_TRUE(count == 1, test_name, "canonicalized results should ignore trailing spaces");
    return true;
}

static bool test_completion_tracker_max_results() {
    const char* test_name = "completion_tracker_max_results";
    std::string error;
    EXPECT_FALSE(completion_tracker::set_completion_max_results(0, &error), test_name,
                 "setting max results below minimum should fail");
    EXPECT_TRUE(!error.empty(), test_name, "error message should be populated");

    long default_max = completion_tracker::get_completion_default_max_results();
    long min_allowed = completion_tracker::get_completion_min_allowed_results();
    EXPECT_TRUE(completion_tracker::set_completion_max_results(min_allowed, nullptr), test_name,
                "setting minimum max results should succeed");
    EXPECT_TRUE(completion_tracker::get_completion_max_results() == min_allowed, test_name,
                "configured max results should match requested value");

    g_completion_actions = {
        {"one", 0, 0, "test"},
        {"two", 0, 0, "test"},
        {"three", 0, 0, "test"},
    };
    ssize_t count = run_completion_generation("", &completion_action_completer, 64);
    g_completion_actions.clear();

    EXPECT_TRUE(count == min_allowed, test_name, "completion count should honor max results cap");

    (void)completion_tracker::set_completion_max_results(default_max, nullptr);
    return true;
}

static bool test_completion_tracker_delete_before_bounds() {
    const char* test_name = "completion_tracker_delete_before_bounds";
    completion_tracker::CompletionTracker tracker(nullptr, "abc");

    if (!expect_streq(tracker.calculate_final_result("z", 2), "az", test_name,
                      "delete_before within bounds should trim from prefix")) {
        return false;
    }
    if (!expect_streq(tracker.calculate_final_result("z", 5), "abcz", test_name,
                      "delete_before beyond prefix length should leave prefix unchanged")) {
        return false;
    }
    if (!expect_streq(tracker.calculate_final_result("z", -1), "abcz", test_name,
                      "negative delete_before should leave prefix unchanged")) {
        return false;
    }

    (void)tracker.added_completions.insert("abcz");
    EXPECT_TRUE(tracker.would_create_duplicate("z", 5), test_name,
                "out-of-range delete_before should still deduplicate canonical result");
    return true;
}

static bool test_completion_apply_consumes_quoted_suffix() {
    const char* test_name = "completion_apply_consumes_quoted_suffix";
    const char* input = "tectonic \"reversi_rl_agent_paper.tex\"";
    ssize_t cursor = static_cast<ssize_t>(std::strlen("tectonic \"re"));
    CompletionAction action = {"\"reversi_rl_agent_paper.tex\"", 3, 0, "test"};
    std::string result;
    ssize_t new_pos = -1;

    EXPECT_TRUE(apply_single_generated_completion(input, cursor, action, result, new_pos),
                test_name, "completion should apply successfully");
    if (!expect_streq(result, input, test_name,
                      "completion should not duplicate the filename suffix after the cursor")) {
        return false;
    }
    EXPECT_TRUE(new_pos == static_cast<ssize_t>(std::strlen(input)), test_name,
                "cursor should move past the completed filename");
    return true;
}

static bool test_completion_apply_moves_over_existing_suffix() {
    const char* test_name = "completion_apply_moves_over_existing_suffix";
    const char* input = "cmd foobar";
    ssize_t cursor = static_cast<ssize_t>(std::strlen("cmd foo"));
    CompletionAction action = {"foobar", 3, 0, "test"};
    std::string result;
    ssize_t new_pos = -1;

    EXPECT_TRUE(apply_single_generated_completion(input, cursor, action, result, new_pos),
                test_name, "completion should apply successfully");
    if (!expect_streq(result, input, test_name,
                      "accepting an existing completion should not change the buffer text")) {
        return false;
    }
    EXPECT_TRUE(new_pos == static_cast<ssize_t>(std::strlen(input)), test_name,
                "cursor should advance over the suffix that was already present");
    return true;
}

static bool test_completion_hint_suppresses_existing_multiline_suffix() {
    const char* test_name = "completion_hint_suppresses_existing_multiline_suffix";
    std::string first_line = "tectonic \"reversi_rl_agent_paper.tex\"";
    std::string second_line =
        "pdftotext \"reversi_rl_agent_paper.pdf\" \"reversi_rl_agent_paper_plain.txt\"";
    std::string input = first_line + "\n" + second_line;

    g_completion_actions = {
        {input, static_cast<long>(first_line.length()), 0, "history"},
    };
    ssize_t count = run_completion_generation_at(
        input.c_str(), static_cast<ssize_t>(first_line.length()), &completion_action_completer, 64);
    g_completion_actions.clear();

    EXPECT_TRUE(count == 1, test_name, "expected one generated completion");

    ic_env_t* env = ic_get_env();
    EXPECT_TRUE(env != nullptr && env->completions != nullptr, test_name,
                "completion environment should be available");
    const char* help = nullptr;
    const char* hint = completions_get_hint(env->completions, 0, &help);
    completions_clear(env->completions);

    EXPECT_TRUE(hint == nullptr, test_name,
                "inline hint should not duplicate multiline text that already follows cursor");
    return true;
}

static bool test_completion_apply_consumes_existing_multiline_suffix() {
    const char* test_name = "completion_apply_consumes_existing_multiline_suffix";
    std::string first_line = "tectonic \"reversi_rl_agent_paper.tex\"";
    std::string second_line =
        "pdftotext \"reversi_rl_agent_paper.pdf\" \"reversi_rl_agent_paper_plain.txt\"";
    std::string input = first_line + "\n" + second_line;
    CompletionAction action = {input, static_cast<long>(first_line.length()), 0, "history"};
    std::string result;
    ssize_t new_pos = -1;

    EXPECT_TRUE(
        apply_single_generated_completion(input.c_str(), static_cast<ssize_t>(first_line.length()),
                                          action, result, new_pos),
        test_name, "completion should apply successfully");
    if (!expect_streq(result, input, test_name,
                      "completion should not duplicate an existing following line")) {
        return false;
    }
    EXPECT_TRUE(new_pos == static_cast<ssize_t>(input.length()), test_name,
                "cursor should move past the existing multiline suffix");
    return true;
}

static bool has_entry(const builtin_completions::CommandDoc* doc, const std::string& text,
                      builtin_completions::EntryKind kind) {
    if (doc == nullptr) {
        return false;
    }
    return std::any_of(doc->entries.begin(), doc->entries.end(),
                       [&](const auto& entry) { return entry.text == text && entry.kind == kind; });
}

static const completion_specs::CompletionEntry* find_spec_entry(
    const std::vector<completion_specs::CompletionEntry>& entries, const std::string& text,
    completion_specs::EntryKind kind) {
    for (const auto& entry : entries) {
        if (entry.kind == kind && completion_specs::entry_matches_token(entry, text)) {
            return &entry;
        }
    }
    return nullptr;
}

static bool test_completion_spec_round_trip() {
    const char* test_name = "completion_spec_round_trip";
    using namespace completion_specs;

    CompletionEntry output{"--output", "Write a result, preserving 100% and\ttabs",
                           EntryKind::Option};
    output.aliases = {"-o"};
    output.value.requirement = ValueRequirement::Required;
    output.value.type = ValueType::File;
    output.value.separator = ValueSeparator::Either;
    output.value.name = "FILE";
    output.value.dynamic_provider = "project-files";
    output.conflicts = {"--stdout"};
    output.dependencies = {"--format"};
    output.repeatable = true;
    output.deprecated = true;

    CompletionEntry nested_option{"--mode", "Select mode", EntryKind::Option};
    nested_option.value.requirement = ValueRequirement::Optional;
    nested_option.value.type = ValueType::Enum;
    nested_option.value.separator = ValueSeparator::Equals;
    nested_option.value.name = "MODE";
    nested_option.value.choices = {"fast", "safe,checked"};

    CompletionEntry remote{"remote", "Manage remotes", EntryKind::Subcommand};
    remote.aliases = {"rem"};
    remote.children.push_back(nested_option);

    CompletionEntry positional{"TARGET", "Destination", EntryKind::Positional};
    positional.value.requirement = ValueRequirement::Required;
    positional.value.type = ValueType::Branch;
    positional.positional_index = 2;
    positional.variadic = true;

    CommandDoc doc;
    doc.summary = "A rich completion spec";
    doc.summary_present = true;
    doc.executable_path = "/usr/bin/example";
    doc.entries = {output, remote, positional};

    std::string serialized = serialize_command_doc("example", doc);
    EXPECT_TRUE(serialized.find("format: 2") != std::string::npos, test_name,
                "serialized spec should declare format version 2");
    auto parsed = parse_command_doc("example", serialized);
    EXPECT_TRUE(parsed.has_value(), test_name, "serialized spec should parse");
    EXPECT_TRUE(parsed->summary == doc.summary, test_name, "summary should round trip");
    EXPECT_TRUE(parsed->executable_path == doc.executable_path, test_name,
                "executable path should round trip");

    const auto* parsed_output = find_spec_entry(parsed->entries, "-o", EntryKind::Option);
    EXPECT_TRUE(parsed_output != nullptr, test_name, "option alias should round trip");
    EXPECT_TRUE(parsed_output->text == "--output", test_name, "canonical option should round trip");
    EXPECT_TRUE(parsed_output->value.requirement == ValueRequirement::Required, test_name,
                "required value should round trip");
    EXPECT_TRUE(parsed_output->value.type == ValueType::File, test_name,
                "file value type should round trip");
    EXPECT_TRUE(parsed_output->value.separator == ValueSeparator::Either, test_name,
                "value separator should round trip");
    EXPECT_TRUE(parsed_output->conflicts == output.conflicts, test_name,
                "conflicts should round trip");
    EXPECT_TRUE(parsed_output->dependencies == output.dependencies, test_name,
                "dependencies should round trip");
    EXPECT_TRUE(parsed_output->repeatable && parsed_output->deprecated, test_name,
                "repeatability and deprecation should round trip");

    const auto* parsed_remote = find_spec_entry(parsed->entries, "rem", EntryKind::Subcommand);
    EXPECT_TRUE(parsed_remote != nullptr && parsed_remote->children.size() == 1, test_name,
                "nested command tree should round trip");
    EXPECT_TRUE(parsed_remote->children[0].value.choices.size() == 2, test_name,
                "nested enum choices should round trip");
    EXPECT_TRUE(parsed_remote->children[0].value.choices[1] == "safe,checked", test_name,
                "commas in enum choices should round trip");

    const auto* parsed_positional =
        find_spec_entry(parsed->entries, "TARGET", EntryKind::Positional);
    EXPECT_TRUE(parsed_positional != nullptr && parsed_positional->positional_index == 2 &&
                    parsed_positional->variadic,
                test_name, "positional metadata should round trip");
    return true;
}

static bool test_completion_spec_legacy_compatibility() {
    const char* test_name = "completion_spec_legacy_compatibility";
    const std::string legacy =
        "generated by cjsh from man page for legacy\n"
        "summary: completes 100% of old entries\n"
        "O\t--force\tForce work\n"
        "S\trun\tRun work\n";
    auto parsed = completion_specs::parse_command_doc("legacy", legacy);
    EXPECT_TRUE(parsed.has_value(), test_name, "legacy cache should parse");
    EXPECT_TRUE(parsed->summary == "completes 100% of old entries", test_name,
                "legacy percent signs should remain literal");
    const auto* option =
        find_spec_entry(parsed->entries, "--force", completion_specs::EntryKind::Option);
    EXPECT_TRUE(option != nullptr && option->repeatable, test_name,
                "legacy options should preserve their previous repeatable behavior");
    EXPECT_TRUE(
        find_spec_entry(parsed->entries, "run", completion_specs::EntryKind::Subcommand) != nullptr,
        test_name, "legacy subcommand should parse");
    return true;
}

static bool test_man_page_value_metadata() {
    const char* test_name = "man_page_value_metadata";
    const std::string man_text =
        "NAME\n"
        "    sample - exercise rich completion parsing\n\n"
        "OPTIONS\n"
        "    -o FILE, --output=FILE  Write output to FILE\n"
        "    --color[=WHEN]          Control color output\n"
        "    --mode={fast,safe}      Select execution mode\n";

    auto doc = parse_man_page_completion_spec("sample", man_text);
    const auto* output = find_spec_entry(doc.entries, "-o", completion_specs::EntryKind::Option);
    EXPECT_TRUE(output != nullptr && output->text == "--output", test_name,
                "short and long options should be represented as aliases");
    EXPECT_TRUE(output->value.requirement == completion_specs::ValueRequirement::Required,
                test_name, "FILE should be preserved as a required value");
    EXPECT_TRUE(output->value.type == completion_specs::ValueType::File, test_name,
                "FILE should infer the file value type");
    EXPECT_TRUE(output->value.name == "FILE", test_name, "FILE metavar should be retained");
    EXPECT_TRUE(output->value.separator == completion_specs::ValueSeparator::Either, test_name,
                "mixed short/long value syntax should accept spaces or equals");

    const auto* color =
        find_spec_entry(doc.entries, "--color", completion_specs::EntryKind::Option);
    EXPECT_TRUE(color != nullptr &&
                    color->value.requirement == completion_specs::ValueRequirement::Optional &&
                    color->value.separator == completion_specs::ValueSeparator::Equals,
                test_name, "optional equals value should be retained");

    const auto* mode = find_spec_entry(doc.entries, "--mode", completion_specs::EntryKind::Option);
    EXPECT_TRUE(mode != nullptr && mode->value.type == completion_specs::ValueType::Enum, test_name,
                "choice lists should infer enum values");
    EXPECT_TRUE(mode->value.choices == std::vector<std::string>({"fast", "safe"}), test_name,
                "enum choices should be retained");
    return true;
}

static bool test_rich_completion_runtime() {
    const char* test_name = "rich_completion_runtime";
    using namespace completion_specs;

    CompletionEntry output{"--output", "Output format", EntryKind::Option};
    output.aliases = {"-o"};
    output.value.requirement = ValueRequirement::Required;
    output.value.type = ValueType::Enum;
    output.value.name = "FORMAT";
    output.value.choices = {"json", "text"};
    output.conflicts = {"--stdout"};

    CompletionEntry stdout_option{"--stdout", "Write to stdout", EntryKind::Option};
    CompletionEntry color{"--color", "Color output", EntryKind::Option};
    color.value.requirement = ValueRequirement::Optional;
    color.value.type = ValueType::Enum;
    color.value.separator = ValueSeparator::Equals;
    color.value.name = "WHEN";
    color.value.choices = {"always", "auto", "never"};
    CompletionEntry compress{"--compress", "Compress output", EntryKind::Option};
    compress.dependencies = {"--output"};
    CompletionEntry old{"--old", "Legacy option", EntryKind::Option};
    old.deprecated = true;

    CompletionEntry branch{"BRANCH", "Git branch", EntryKind::Positional};
    branch.value.requirement = ValueRequirement::Required;
    branch.value.type = ValueType::Branch;
    branch.value.dynamic_provider = "completion-test-branches";
    CompletionEntry remote{"remote", "Manage remotes", EntryKind::Subcommand};
    remote.children = {branch};

    CommandDoc doc;
    doc.summary = "Runtime completion test";
    doc.entries = {output, stdout_option, color, compress, old, remote};

    EXPECT_TRUE(register_command_doc("richspec-test", doc), test_name,
                "runtime command spec should register");
    EXPECT_TRUE(
        register_dynamic_completion_provider(
            "completion-test-branches",
            [](const DynamicCompletionRequest& request) {
                if (request.command_path.size() != 2 || request.command_path[1] != "remote") {
                    return std::vector<DynamicCompletionCandidate>{};
                }
                return std::vector<DynamicCompletionCandidate>{{"main", "default branch"},
                                                               {"feature", "topic branch"}};
            }),
        test_name, "dynamic provider should register");

    (void)run_completion_generation("richspec-test --out", &cjsh_default_completer, 256);
    bool has_option = generated_completions_include_replacement("--output ");
    clear_generated_completions();
    EXPECT_TRUE(has_option, test_name, "canonical rich option should complete");

    (void)run_completion_generation("richspec-test --output j", &cjsh_default_completer, 256);
    bool has_enum = generated_completions_include_replacement("json ");
    clear_generated_completions();
    EXPECT_TRUE(has_enum, test_name, "required enum value should complete");

    (void)run_completion_generation("richspec-test --output=j", &cjsh_default_completer, 256);
    bool has_inline_enum = generated_completions_include_replacement("--output=json ");
    clear_generated_completions();
    EXPECT_TRUE(has_inline_enum, test_name, "inline enum value should complete");

    (void)run_completion_generation("richspec-test --col", &cjsh_default_completer, 256);
    bool has_optional_option = generated_completions_include_replacement("--color=");
    clear_generated_completions();
    EXPECT_TRUE(has_optional_option, test_name,
                "optional equals value should retain its separator");

    (void)run_completion_generation("richspec-test --color=a", &cjsh_default_completer, 256);
    bool has_optional_value = generated_completions_include_replacement("--color=always ");
    clear_generated_completions();
    EXPECT_TRUE(has_optional_value, test_name, "optional enum value should complete");

    (void)run_completion_generation("richspec-test --c", &cjsh_default_completer, 256);
    bool dependency_hidden = !generated_completions_include_replacement("--compress ");
    clear_generated_completions();
    EXPECT_TRUE(dependency_hidden, test_name, "unsatisfied dependency should hide an option");

    (void)run_completion_generation("richspec-test --output json --c", &cjsh_default_completer,
                                    256);
    bool dependency_visible = generated_completions_include_replacement("--compress ");
    clear_generated_completions();
    EXPECT_TRUE(dependency_visible, test_name, "satisfied dependency should expose an option");

    (void)run_completion_generation("richspec-test --stdout --o", &cjsh_default_completer, 256);
    bool conflict_hidden = !generated_completions_include_replacement("--output ");
    clear_generated_completions();
    EXPECT_TRUE(conflict_hidden, test_name, "conflicting option should be hidden");

    (void)run_completion_generation("richspec-test --output json --s", &cjsh_default_completer,
                                    256);
    bool reverse_conflict_hidden = !generated_completions_include_replacement("--stdout ");
    clear_generated_completions();
    EXPECT_TRUE(reverse_conflict_hidden, test_name,
                "a declared conflict should be enforced in both directions");

    (void)run_completion_generation("richspec-test --stdout --s", &cjsh_default_completer, 256);
    bool repeated_hidden = !generated_completions_include_replacement("--stdout ");
    clear_generated_completions();
    EXPECT_TRUE(repeated_hidden, test_name, "non-repeatable option should be hidden after use");

    (void)run_completion_generation("richspec-test --output json remote f", &cjsh_default_completer,
                                    256);
    bool dynamic_nested = generated_completions_include_replacement("feature ");
    clear_generated_completions();
    EXPECT_TRUE(dynamic_nested, test_name,
                "nested positional should invoke its provider after a global option value");

    (void)run_completion_generation("richspec-test --ol", &cjsh_default_completer, 256);
    bool deprecated_labeled = generated_completions_include_source("deprecated · Legacy option");
    clear_generated_completions();
    EXPECT_TRUE(deprecated_labeled, test_name, "deprecated candidates should be labeled");

    EXPECT_TRUE(unregister_dynamic_completion_provider("completion-test-branches"), test_name,
                "dynamic provider should unregister");
    EXPECT_TRUE(unregister_command_doc("richspec-test"), test_name,
                "runtime command spec should unregister");
    return true;
}

static bool test_hints_defer_documentation_fetch() {
    const char* test_name = "hints_defer_documentation_fetch";
    namespace fs = std::filesystem;
    const fs::path root = cjsh_filesystem::g_user_home_path() / "hint-fetch";
    fs::create_directories(root);
    std::ofstream(root / "hintfetch-fixture") << "#!/bin/sh\nexit 0\n";
    std::ofstream(root / "unrelated-command-in-path") << "#!/bin/sh\nexit 0\n";
    std::ofstream(root / "man-fixture")
        << "#!/bin/sh\nprintf 'called\\n' >> \"$CJSH_TEST_MAN_LOG\"\n"
           "printf 'NAME\\n    hintfetch-fixture - fixture command\\n"
           "OPTIONS\\n    --sample    Sample option\\n'\n";
    fs::permissions(root / "hintfetch-fixture", fs::perms::owner_all);
    fs::permissions(root / "unrelated-command-in-path", fs::perms::owner_all);
    fs::permissions(root / "man-fixture", fs::perms::owner_all);
    std::ofstream(root / "hintfetch-nonexecutable") << "plain file\n";
    const ScopedEnvironmentValue path("PATH", root.string());
    const ScopedEnvironmentValue man_path("CJSH_MAN_PATH", (root / "man-fixture").string());
    const ScopedEnvironmentValue man_log("CJSH_TEST_MAN_LOG", (root / "calls").string());
    const bool previous_learning = config::completion_learning_enabled;
    config::completion_learning_enabled = true;

    (void)run_hint_generation("hintfetch-fixtu");
    const bool hinted_command = generated_completions_include_replacement("hintfetch-fixture ");
    const auto hashed_commands = cjsh_filesystem::get_path_hash_entries();
    const bool skipped_unrelated = std::none_of(
        hashed_commands.begin(), hashed_commands.end(),
        [](const auto& entry) { return entry.command == "unrelated-command-in-path"; });
    (void)run_hint_generation("hintfetch-nonexecutable");
    const bool rejected_nonexecutable =
        !generated_completions_include_replacement("hintfetch-nonexecutable ");
    (void)run_hint_generation("hintfetch-fixture --sam");
    const bool deferred = !fs::exists(root / "calls");
    (void)run_completion_generation("hintfetch-fixtu", &cjsh_default_completer, 256);
    const bool fetched = fs::exists(root / "calls");
    (void)run_hint_generation("hintfetch-fixture --sam");
    const bool hinted_option = generated_completions_include_replacement("--sample ");
    config::completion_learning_enabled = previous_learning;
    clear_generated_completions();

    EXPECT_TRUE(hinted_command, test_name, "cold hints should still find executable commands");
    EXPECT_TRUE(skipped_unrelated, test_name,
                "cold hints must not eagerly hash every PATH executable");
    EXPECT_TRUE(rejected_nonexecutable, test_name,
                "name-only scanning must check candidate permissions");
    EXPECT_TRUE(deferred, test_name, "typing command names and arguments must not launch man");
    EXPECT_TRUE(fetched, test_name, "Tab must still fetch documentation after a cache-only hint");
    EXPECT_TRUE(hinted_option, test_name, "hints should use documentation once it is cached");
    return true;
}

static bool test_tab_path_candidates_and_refresh() {
    const char* test_name = "tab_path_candidates_and_refresh";
    namespace fs = std::filesystem;
    const fs::path root = cjsh_filesystem::g_user_home_path() / "tab-path";
    fs::create_directories(root);
    auto executable = [&](const char* name) {
        std::ofstream(root / name) << "#!/bin/sh\nexit 0\n";
        fs::permissions(root / name, fs::perms::owner_all);
    };
    executable("tabfixture-tool");
    executable("unrelated-command-in-path");
    std::ofstream(root / "tabfixture-plain") << "plain file\n";
    fs::create_directory(root / "tabfixture-directory");
    fs::create_symlink(root / "tabfixture-tool", root / "tabfixture-link");
    fs::create_symlink(root / "missing", root / "tabfixture-broken");
    const ScopedEnvironmentValue path("PATH", root.string());
    const bool previous_learning = config::completion_learning_enabled;
    config::completion_learning_enabled = false;

    (void)run_completion_generation("tabfixture-", &cjsh_default_completer, 256);
    const bool found_tool = generated_completions_include_replacement("tabfixture-tool ");
    const bool found_link = generated_completions_include_replacement("tabfixture-link ");
    const bool rejected_invalid =
        !generated_completions_include_replacement("tabfixture-plain ") &&
        !generated_completions_include_replacement("tabfixture-directory ") &&
        !generated_completions_include_replacement("tabfixture-broken ");
    const auto hashed_commands = cjsh_filesystem::get_path_hash_entries();
    const bool skipped_unrelated = std::none_of(
        hashed_commands.begin(), hashed_commands.end(),
        [](const auto& entry) { return entry.command == "unrelated-command-in-path"; });

    (void)run_hint_generation("tabfixture-new");
    executable("tabfixture-new");
    fs::permissions(root / "tabfixture-tool", fs::perms::owner_read);
    fs::permissions(root / "tabfixture-plain", fs::perms::owner_all);
    (void)run_completion_generation("tabfixture-", &cjsh_default_completer, 256);
    const bool refreshed = generated_completions_include_replacement("tabfixture-new ") &&
                           generated_completions_include_replacement("tabfixture-plain ") &&
                           !generated_completions_include_replacement("tabfixture-tool ") &&
                           !generated_completions_include_replacement("tabfixture-link ");
    config::completion_learning_enabled = previous_learning;
    clear_generated_completions();

    EXPECT_TRUE(found_tool && found_link, test_name,
                "Tab must find executables and follow executable symlinks");
    EXPECT_TRUE(rejected_invalid, test_name,
                "Tab must reject nonexecutables, directories, and broken symlinks in PATH");
    EXPECT_TRUE(skipped_unrelated, test_name,
                "Tab must not eagerly hash unrelated PATH executables");
    EXPECT_TRUE(refreshed, test_name,
                "Tab must discover new commands and recheck permissions after cached hints");
    return true;
}

static bool test_hints_defer_dynamic_providers() {
    const char* test_name = "hints_defer_dynamic_providers";
    using namespace completion_specs;
    CompletionEntry value{"--value", "Select value", EntryKind::Option};
    value.value.requirement = ValueRequirement::Required;
    value.value.choices = {"static-choice"};
    value.value.dynamic_provider = "hint-provider-fixture";
    CommandDoc doc;
    doc.entries = {value};
    int provider_calls = 0;
    (void)register_command_doc("hint-provider-command", doc);
    (void)register_dynamic_completion_provider(
        "hint-provider-fixture", [&](const DynamicCompletionRequest&) {
            ++provider_calls;
            return std::vector<DynamicCompletionCandidate>{{"dynamic-choice", "fixture"}};
        });
    (void)run_hint_generation("hint-provider-command --value s");
    const bool static_hint = generated_completions_include_replacement("static-choice ");
    (void)run_hint_generation("hint-provider-command --value d");
    const bool deferred = provider_calls == 0;
    (void)run_completion_generation("hint-provider-command --value d", &cjsh_default_completer,
                                    256);
    const bool dynamic_tab =
        provider_calls > 0 && generated_completions_include_replacement("dynamic-choice ");
    (void)unregister_dynamic_completion_provider("hint-provider-fixture");
    (void)unregister_command_doc("hint-provider-command");
    clear_generated_completions();
    EXPECT_TRUE(static_hint, test_name, "hints should retain static value choices");
    EXPECT_TRUE(deferred, test_name, "typing must not invoke dynamic completion providers");
    EXPECT_TRUE(dynamic_tab, test_name, "Tab must still invoke dynamic completion providers");
    return true;
}

static bool test_command_context_completion_runtime() {
    const char* test_name = "command_context_completion_runtime";
    using namespace completion_specs;

    CompletionEntry output{"--output", "Output format", EntryKind::Option};
    output.value.requirement = ValueRequirement::Required;
    output.value.type = ValueType::Enum;
    output.value.choices = {"json", "text"};

    CompletionEntry detach{"--detach", "Detach checkout", EntryKind::Option};
    CompletionEntry checkout{"checkout", "Switch branches", EntryKind::Subcommand};
    checkout.children = {detach};

    CommandDoc doc;
    doc.entries = {output, checkout};
    EXPECT_TRUE(register_command_doc("context-test", doc), test_name,
                "runtime context command should register");

    auto expect_completion = [&](const char* input, const char* replacement, const char* message) {
        (void)run_completion_generation(input, &cjsh_default_completer, 256);
        bool found = generated_completions_include_replacement(replacement);
        clear_generated_completions();
        if (!found) {
            log_failure(test_name, message);
        }
        return found;
    };

    EXPECT_TRUE(expect_completion("context-test --output json checkout --d", "--detach ",
                                  "options before a subcommand should preserve nested state"),
                test_name, "nested option completion should be generated");
    EXPECT_TRUE(expect_completion("printf x | context-test --out", "--output ",
                                  "pipeline state should begin after the last pipe"),
                test_name, "pipeline completion should be generated");
    EXPECT_TRUE(expect_completion("FOO=bar context-test --out", "--output ",
                                  "leading assignments should not become the command"),
                test_name, "assignment-prefixed completion should be generated");
    EXPECT_TRUE(expect_completion("sudo -u root context-test --out", "--output ",
                                  "sudo options and their values should be skipped"),
                test_name, "sudo-wrapped completion should be generated");
    EXPECT_TRUE(
        expect_completion("env -u OLD FOO=\"x y\" command -- context-test --out", "--output ",
                          "chained env and command wrappers should resolve the effective command"),
        test_name, "wrapper-chain completion should be generated");
    EXPECT_TRUE(expect_completion("\"context-test\" --output \"j", "json ",
                                  "quoted commands and values should use decoded matching"),
                test_name, "quoted value completion should be generated");

    (void)run_completion_generation("context-test -- --out", &cjsh_default_completer, 256);
    bool option_after_separator = generated_completions_include_replacement("--output ");
    clear_generated_completions();
    EXPECT_FALSE(option_after_separator, test_name,
                 "the option separator should disable later option completion");

    const std::string midline = "echo ignored | context-test --out trailing";
    const std::size_t cursor = midline.find(" trailing");
    (void)run_completion_generation_at(midline.c_str(), static_cast<ssize_t>(cursor),
                                       &cjsh_default_completer, 256);
    bool midline_option = generated_completions_include_replacement("--output ");
    clear_generated_completions();
    EXPECT_TRUE(midline_option, test_name,
                "only input before the cursor should drive completion state");

    EXPECT_TRUE(expect_completion("sudo ec", "echo ",
                                  "a wrapper command operand should get command completions"),
                test_name, "wrapper command completion should be generated");
    EXPECT_TRUE(expect_completion("printf x | ec", "echo ",
                                  "a pipeline command position should get command completions"),
                test_name, "pipeline command completion should be generated");

    EXPECT_TRUE(unregister_command_doc("context-test"), test_name,
                "runtime context command should unregister");
    return true;
}

static bool test_builtin_docs() {
    const char* test_name = "builtin_docs";

    const auto* cjsh_doc = builtin_completions::lookup_builtin_command_doc("cjsh");
    EXPECT_TRUE(cjsh_doc != nullptr, test_name, "cjsh doc should exist");
    EXPECT_TRUE(cjsh_doc->summary_present, test_name, "cjsh summary should be present");
    EXPECT_TRUE(has_entry(cjsh_doc, "--help", builtin_completions::EntryKind::Option), test_name,
                "cjsh doc should include --help option");
    EXPECT_TRUE(has_entry(cjsh_doc, "--no-history", builtin_completions::EntryKind::Option),
                test_name, "cjsh doc should include --no-history option");
    EXPECT_TRUE(has_entry(cjsh_doc, "--no-agent", builtin_completions::EntryKind::Option),
                test_name, "cjsh doc should include --no-agent option");

    const auto* hook_doc = builtin_completions::lookup_builtin_command_doc("hook");
    EXPECT_TRUE(hook_doc != nullptr, test_name, "hook doc should exist");
    EXPECT_TRUE(has_entry(hook_doc, "add", builtin_completions::EntryKind::Subcommand), test_name,
                "hook doc should include add subcommand");
    EXPECT_TRUE(has_entry(hook_doc, "remove", builtin_completions::EntryKind::Subcommand),
                test_name, "hook doc should include remove subcommand");
    EXPECT_TRUE(has_entry(hook_doc, "list", builtin_completions::EntryKind::Subcommand), test_name,
                "hook doc should include list subcommand");
    EXPECT_TRUE(has_entry(hook_doc, "clear", builtin_completions::EntryKind::Subcommand), test_name,
                "hook doc should include clear subcommand");

    const auto* abbreviate_doc = builtin_completions::lookup_builtin_command_doc("abbreviate");
    EXPECT_TRUE(abbreviate_doc != nullptr, test_name, "alias doc should be available");
    if (!expect_streq(abbreviate_doc->summary, "Manage interactive abbreviations", test_name,
                      "alias summary should match base command")) {
        return false;
    }

    const auto* generate_doc =
        builtin_completions::lookup_builtin_command_doc("generate-completions");
    EXPECT_TRUE(generate_doc != nullptr, test_name, "generate-completions doc should exist");
    EXPECT_TRUE(has_entry(generate_doc, "--no-force", builtin_completions::EntryKind::Option),
                test_name, "generate-completions should include --no-force");
    EXPECT_TRUE(has_entry(generate_doc, "--jobs", builtin_completions::EntryKind::Option),
                test_name, "generate-completions should include --jobs");
    const auto* jobs_entry =
        find_spec_entry(generate_doc->entries, "-j", completion_specs::EntryKind::Option);
    EXPECT_TRUE(jobs_entry != nullptr && jobs_entry->text == "--jobs", test_name,
                "generate-completions should model -j as an alias");
    EXPECT_TRUE(jobs_entry->value.requirement == completion_specs::ValueRequirement::Required &&
                    jobs_entry->value.name == "JOBS",
                test_name, "generate-completions should model the --jobs value");
    EXPECT_TRUE(has_entry(generate_doc, "--subcommands", builtin_completions::EntryKind::Option),
                test_name, "generate-completions should include --subcommands");

    const auto* source_doc = builtin_completions::lookup_builtin_command_doc(".");
    EXPECT_TRUE(source_doc != nullptr, test_name, "dot alias doc should exist");
    EXPECT_TRUE(source_doc->summary == "Execute commands from a file in the current shell",
                test_name, "dot alias should share source summary");

    const auto* approot_doc = builtin_completions::lookup_builtin_command_doc("approot");
    EXPECT_TRUE(approot_doc != nullptr, test_name, "approot doc should exist");
    EXPECT_TRUE(has_entry(approot_doc, "--print", builtin_completions::EntryKind::Option),
                test_name, "approot should include --print option");
    EXPECT_TRUE(has_entry(approot_doc, "--file", builtin_completions::EntryKind::Option), test_name,
                "approot should include --file option");
    EXPECT_TRUE(has_entry(approot_doc, "config", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include config target");
    EXPECT_TRUE(has_entry(approot_doc, "history", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include history target");
    EXPECT_TRUE(has_entry(approot_doc, "firstboot", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include firstboot target");
    EXPECT_TRUE(has_entry(approot_doc, "first_boot", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include first_boot target");
    EXPECT_TRUE(has_entry(approot_doc, "cjshrc", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include cjshrc target");
    EXPECT_TRUE(has_entry(approot_doc, "cjsh", builtin_completions::EntryKind::Subcommand),
                test_name, "approot should include cjsh target");

    const auto* firstboot_doc = builtin_completions::lookup_builtin_command_doc("firstboot");
    if (cjsh_filesystem::is_first_boot()) {
        EXPECT_TRUE(firstboot_doc != nullptr, test_name,
                    "firstboot doc should exist before its marker is created");
    } else {
        EXPECT_TRUE(firstboot_doc == nullptr, test_name,
                    "firstboot doc should be unavailable after its marker is created");
    }

    const auto* cjshopt_doc = builtin_completions::lookup_builtin_command_doc("cjshopt");
    EXPECT_TRUE(cjshopt_doc != nullptr, test_name, "cjshopt doc should exist");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "completion-case", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include completion-case subcommand");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "exit-confirmation", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include exit-confirmation subcommand");
    EXPECT_TRUE(has_entry(cjshopt_doc, "extglob", builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include extglob subcommand");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "mouse-clicking", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include mouse-clicking subcommand");
    EXPECT_TRUE(has_entry(cjshopt_doc, "mouse-clicking-status-line",
                          builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include mouse-clicking-status-line subcommand");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "status-line-callback", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include status-line-callback subcommand");
    EXPECT_TRUE(has_entry(cjshopt_doc, "agent-mode", builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include agent-mode subcommand");
    EXPECT_TRUE(!has_entry(cjshopt_doc, "completion-menu-expanded",
                           builtin_completions::EntryKind::Subcommand),
                test_name,
                "cjshopt should not include the removed completion-menu-expanded setting");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "completion-auto-menu", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include completion-auto-menu subcommand");
    EXPECT_TRUE(has_entry(cjshopt_doc, "completion-click-accept",
                          builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include completion-click-accept subcommand");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "menu-highlighting", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include menu-highlighting subcommand");
    EXPECT_TRUE(has_entry(cjshopt_doc, "completion-spell-enter",
                          builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include completion-spell-enter subcommand");
    EXPECT_TRUE(
        has_entry(cjshopt_doc, "multiline-max-lines", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should include multiline-max-lines subcommand");

    const auto* multiline_max_lines_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-multiline-max-lines");
    EXPECT_TRUE(multiline_max_lines_doc != nullptr, test_name,
                "cjshopt-multiline-max-lines doc should exist");
    EXPECT_TRUE(
        has_entry(multiline_max_lines_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "multiline-max-lines should include status subcommand");

    for (const std::string command : {"completion-menu-max-lines", "history-menu-max-lines",
                                      "command-palette-max-lines", "custom-menu-max-lines"}) {
        EXPECT_TRUE(has_entry(cjshopt_doc, command, builtin_completions::EntryKind::Subcommand),
                    test_name, "cjshopt should include each per-menu height subcommand");
        const auto* menu_doc =
            builtin_completions::lookup_builtin_command_doc("cjshopt-" + command);
        EXPECT_TRUE(menu_doc != nullptr, test_name, "per-menu height documentation should exist");
        EXPECT_TRUE(has_entry(menu_doc, "status", builtin_completions::EntryKind::Subcommand),
                    test_name, "per-menu height subcommands should complete status");
    }

    EXPECT_TRUE(
        !has_entry(cjshopt_doc, "menu-max-lines", builtin_completions::EntryKind::Subcommand),
        test_name, "cjshopt should not include the removed menu-max-lines subcommand");
    EXPECT_TRUE(builtin_completions::lookup_builtin_command_doc("cjshopt-menu-max-lines") == nullptr,
                test_name, "removed menu-max-lines documentation should not exist");

    const auto* exit_confirmation_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-exit-confirmation");
    EXPECT_TRUE(exit_confirmation_doc != nullptr, test_name,
                "cjshopt-exit-confirmation doc should exist");
    EXPECT_TRUE(
        has_entry(exit_confirmation_doc, "smart", builtin_completions::EntryKind::Subcommand),
        test_name, "exit-confirmation should include smart mode");
    EXPECT_TRUE(
        has_entry(exit_confirmation_doc, "always", builtin_completions::EntryKind::Subcommand),
        test_name, "exit-confirmation should include always mode");
    EXPECT_TRUE(
        has_entry(exit_confirmation_doc, "never", builtin_completions::EntryKind::Subcommand),
        test_name, "exit-confirmation should include never mode");
    EXPECT_TRUE(
        has_entry(exit_confirmation_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "exit-confirmation should include status subcommand");

    const auto* agent_mode_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-agent-mode");
    EXPECT_TRUE(agent_mode_doc != nullptr, test_name, "cjshopt-agent-mode doc should exist");
    EXPECT_TRUE(has_entry(agent_mode_doc, "set", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include set subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "key", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include key subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "clear", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include clear subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "list", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include list subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "status", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include status subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "on", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include on subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "off", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include off subcommand");
    EXPECT_TRUE(has_entry(agent_mode_doc, "reset", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode should include reset subcommand");

    const auto* agent_mode_set_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-agent-mode-set");
    EXPECT_TRUE(agent_mode_set_doc != nullptr, test_name,
                "cjshopt-agent-mode-set doc should exist");
    EXPECT_TRUE(has_entry(agent_mode_set_doc, "--command", builtin_completions::EntryKind::Option),
                test_name, "agent-mode set should include --command");
    EXPECT_TRUE(
        has_entry(agent_mode_set_doc, "--system-prompt", builtin_completions::EntryKind::Option),
        test_name, "agent-mode set should include --system-prompt");
    EXPECT_TRUE(
        has_entry(agent_mode_set_doc, "--trigger-prefix", builtin_completions::EntryKind::Option),
        test_name, "agent-mode set should include --trigger-prefix");

    const auto* agent_mode_key_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-agent-mode-key");
    EXPECT_TRUE(agent_mode_key_doc != nullptr, test_name,
                "cjshopt-agent-mode-key doc should exist");
    EXPECT_TRUE(
        has_entry(agent_mode_key_doc, "default", builtin_completions::EntryKind::Subcommand),
        test_name, "agent-mode key should include default");
    EXPECT_TRUE(has_entry(agent_mode_key_doc, "off", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode key should include off");
    EXPECT_TRUE(has_entry(agent_mode_key_doc, "status", builtin_completions::EntryKind::Subcommand),
                test_name, "agent-mode key should include status");

    const auto* agent_mode_clear_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-agent-mode-clear");
    EXPECT_TRUE(agent_mode_clear_doc != nullptr, test_name,
                "cjshopt-agent-mode-clear doc should exist");
    EXPECT_TRUE(
        has_entry(agent_mode_clear_doc, "--default", builtin_completions::EntryKind::Option),
        test_name, "agent-mode clear should include --default");
    EXPECT_TRUE(
        has_entry(agent_mode_clear_doc, "--trigger-prefix", builtin_completions::EntryKind::Option),
        test_name, "agent-mode clear should include --trigger-prefix");
    EXPECT_TRUE(has_entry(agent_mode_clear_doc, "--all", builtin_completions::EntryKind::Option),
                test_name, "agent-mode clear should include --all");

    EXPECT_TRUE(has_entry(cjshopt_doc, "multiline-bottom-lines",
                          builtin_completions::EntryKind::Subcommand),
                test_name, "cjshopt should include multiline-bottom-lines subcommand");

    const auto* multiline_bottom_lines_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-multiline-bottom-lines");
    EXPECT_TRUE(multiline_bottom_lines_doc != nullptr, test_name,
                "cjshopt-multiline-bottom-lines doc should exist");
    EXPECT_TRUE(
        has_entry(multiline_bottom_lines_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "multiline-bottom-lines should include status subcommand");

    const auto* completion_max_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-set-completion-max");
    EXPECT_TRUE(completion_max_doc != nullptr, test_name,
                "cjshopt-set-completion-max doc should exist");
    EXPECT_TRUE(has_entry(completion_max_doc, "--status", builtin_completions::EntryKind::Option),
                test_name, "set-completion-max should include --status option");

    const auto* mouse_mode_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-mouse-clicking");
    EXPECT_TRUE(mouse_mode_doc != nullptr, test_name, "cjshopt-mouse-clicking doc should exist");
    EXPECT_TRUE(has_entry(mouse_mode_doc, "all-off", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking should include all-off mode");
    EXPECT_TRUE(has_entry(mouse_mode_doc, "off", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking should include menu-only off mode");
    EXPECT_TRUE(has_entry(mouse_mode_doc, "disabled", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking should retain the disabled alias");
    EXPECT_TRUE(has_entry(mouse_mode_doc, "simple", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking should include simple mode");
    EXPECT_TRUE(has_entry(mouse_mode_doc, "smart", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking should include smart mode");

    const auto* mouse_status_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-mouse-clicking-status-line");
    EXPECT_TRUE(mouse_status_doc != nullptr, test_name,
                "cjshopt-mouse-clicking-status-line doc should exist");
    EXPECT_TRUE(has_entry(mouse_status_doc, "status", builtin_completions::EntryKind::Subcommand),
                test_name, "mouse-clicking-status-line should include status subcommand");

    const auto* status_callback_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-status-line-callback");
    EXPECT_TRUE(status_callback_doc != nullptr, test_name,
                "cjshopt-status-line-callback doc should exist");
    EXPECT_TRUE(
        has_entry(status_callback_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "status-line-callback should include status subcommand");

    const auto* completion_menu_expanded_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-completion-menu-expanded");
    EXPECT_TRUE(completion_menu_expanded_doc == nullptr, test_name,
                "removed completion-menu-expanded setting should not have completion docs");

    const auto* completion_auto_menu_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-completion-auto-menu");
    EXPECT_TRUE(completion_auto_menu_doc != nullptr, test_name,
                "cjshopt-completion-auto-menu doc should exist");
    for (const char* option : {"on", "off", "status"}) {
        EXPECT_TRUE(
            has_entry(completion_auto_menu_doc, option, builtin_completions::EntryKind::Subcommand),
            test_name, "completion-auto-menu should include on/off/status");
    }

    const auto* completion_click_accept_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-completion-click-accept");
    EXPECT_TRUE(completion_click_accept_doc != nullptr, test_name,
                "cjshopt-completion-click-accept doc should exist");
    EXPECT_TRUE(
        has_entry(completion_click_accept_doc, "on", builtin_completions::EntryKind::Subcommand),
        test_name, "completion-click-accept should include on subcommand");
    EXPECT_TRUE(
        has_entry(completion_click_accept_doc, "off", builtin_completions::EntryKind::Subcommand),
        test_name, "completion-click-accept should include off subcommand");
    EXPECT_TRUE(has_entry(completion_click_accept_doc, "status",
                          builtin_completions::EntryKind::Subcommand),
                test_name, "completion-click-accept should include status subcommand");

    const auto* menu_highlighting_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-menu-highlighting");
    EXPECT_TRUE(menu_highlighting_doc != nullptr, test_name,
                "cjshopt-menu-highlighting doc should exist");
    EXPECT_TRUE(
        has_entry(menu_highlighting_doc, "none", builtin_completions::EntryKind::Subcommand),
        test_name, "menu-highlighting should include none subcommand");
    EXPECT_TRUE(
        has_entry(menu_highlighting_doc, "single", builtin_completions::EntryKind::Subcommand),
        test_name, "menu-highlighting should include single subcommand");
    EXPECT_TRUE(has_entry(menu_highlighting_doc, "all", builtin_completions::EntryKind::Subcommand),
                test_name, "menu-highlighting should include all subcommand");
    EXPECT_TRUE(
        has_entry(menu_highlighting_doc, "reverse", builtin_completions::EntryKind::Subcommand),
        test_name, "menu-highlighting should include reverse subcommand");
    EXPECT_TRUE(
        has_entry(menu_highlighting_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "menu-highlighting should include status subcommand");

    const auto* completion_spell_enter_doc =
        builtin_completions::lookup_builtin_command_doc("cjshopt-completion-spell-enter");
    EXPECT_TRUE(completion_spell_enter_doc != nullptr, test_name,
                "cjshopt-completion-spell-enter doc should exist");
    EXPECT_TRUE(
        has_entry(completion_spell_enter_doc, "status", builtin_completions::EntryKind::Subcommand),
        test_name, "completion-spell-enter should include status subcommand");

    const auto* type_doc = builtin_completions::lookup_builtin_command_doc("type");
    EXPECT_TRUE(type_doc != nullptr, test_name, "type doc should exist");
    EXPECT_TRUE(has_entry(type_doc, "-a", builtin_completions::EntryKind::Option), test_name,
                "type should include -a option");

    const auto* jobname_doc = builtin_completions::lookup_builtin_command_doc("jobname");
    EXPECT_TRUE(jobname_doc != nullptr, test_name, "jobname doc should exist");
    EXPECT_TRUE(has_entry(jobname_doc, "--clear", builtin_completions::EntryKind::Option),
                test_name, "jobname should include --clear option");

    const auto* missing_doc =
        builtin_completions::lookup_builtin_command_doc("definitely-not-a-real-command");
    EXPECT_TRUE(missing_doc == nullptr, test_name,
                "lookup should return nullptr for unknown commands");

    return true;
}

using test_fn_t = bool (*)();

using test_case_t = struct test_case_s {
    const char* name;
    test_fn_t fn;
};

static const test_case_t kTests[] = {
    {"history_completer_exit_code_ordering", test_history_completer_exit_code_ordering},
    {"empty_prompt_history_ranking", test_empty_prompt_history_ranking},
    {"history_directory_completions", test_history_directory_completions},
    {"history_prefix_metadata_isolation", test_history_prefix_metadata_isolation},
    {"empty_prompt_history_limits", test_empty_prompt_history_limits},
    {"empty_prompt_legacy_history", test_empty_prompt_legacy_history},
    {"empty_prompt_without_history", test_empty_prompt_without_history},
    {"history_metadata_field_boundaries", test_history_metadata_field_boundaries},
    {"quote_and_unquote_paths", test_quote_and_unquote_paths},
    {"quote_path_special_characters", test_quote_path_special_characters},
    {"quote_path_empty_and_dollar", test_quote_path_empty_and_dollar},
    {"unquote_path_with_escaped_quote", test_unquote_path_with_escaped_quote},
    {"unquote_path_with_mixed_quote_segments", test_unquote_path_with_mixed_quote_segments},
    {"tokenize_command_line", test_tokenize_command_line},
    {"tokenize_command_line_escaped_quotes", test_tokenize_command_line_escaped_quotes},
    {"tokenize_command_line_unterminated_quote", test_tokenize_command_line_unterminated_quote},
    {"completion_context_shell_state", test_completion_context_shell_state},
    {"completion_context_cursor_and_quotes", test_completion_context_cursor_and_quotes},
    {"completion_context_wrapper_value_state", test_completion_context_wrapper_value_state},
    {"completion_context_assignment_lhs_at_cursor",
     test_completion_context_assignment_lhs_at_cursor},
    {"completion_context_before_existing_word", test_completion_context_before_existing_word},
    {"tokenize_shell_words_preserve_literals", test_tokenize_shell_words_preserve_literals},
    {"default_completer_command_in_command_substitution",
     test_default_completer_command_in_command_substitution},
    {"default_completer_nested_command_substitution_scope",
     test_default_completer_nested_command_substitution_scope},
    {"default_completer_split_unknown_command_merge",
     test_default_completer_split_unknown_command_merge},
    {"default_completer_spell_follows_command_cursor",
     test_default_completer_spell_follows_command_cursor},
    {"default_completer_does_not_spell_correct_assignments",
     test_default_completer_does_not_spell_correct_assignments},
    {"default_completer_suppresses_assignment_lhs_midline",
     test_default_completer_suppresses_assignment_lhs_midline},
    {"default_completer_suppresses_before_existing_word",
     test_default_completer_suppresses_before_existing_word},
    {"default_completer_suppresses_inside_known_command",
     test_default_completer_suppresses_inside_known_command},
    {"known_shell_command_completion_without_path",
     test_known_shell_command_completion_without_path},
    {"default_completer_keeps_unfinished_command_completions",
     test_default_completer_keeps_unfinished_command_completions},
    {"find_last_unquoted_space", test_find_last_unquoted_space},
    {"find_last_unquoted_space_with_tabs", test_find_last_unquoted_space_with_tabs},
    {"find_last_unquoted_space_with_escaped_space",
     test_find_last_unquoted_space_with_escaped_space},
    {"find_last_unquoted_space_with_unterminated_quote",
     test_find_last_unquoted_space_with_unterminated_quote},
    {"case_sensitivity_helpers", test_case_sensitivity_helpers},
    {"normalize_for_comparison", test_normalize_for_comparison},
    {"starts_with_helpers", test_starts_with_helpers},
    {"sanitize_job_summary", test_sanitize_job_summary},
    {"sanitize_job_summary_truncates", test_sanitize_job_summary_truncates},
    {"sanitize_job_summary_whitespace_only", test_sanitize_job_summary_whitespace_only},
    {"spell_transposition_and_distance", test_spell_transposition_and_distance},
    {"spell_distance_negative_limit", test_spell_distance_negative_limit},
    {"spell_match_ordering", test_spell_match_ordering},
    {"spell_match_add_limit", test_spell_match_add_limit},
    {"collect_spell_candidates_filter_and_case_normalization",
     test_collect_spell_candidates_filter_and_case_normalization},
    {"collect_spell_candidates_distance_thresholds",
     test_collect_spell_candidates_distance_thresholds},
    {"collect_spell_candidates_without_filter", test_collect_spell_candidates_without_filter},
    {"collect_transpositions_only", test_collect_transpositions_only},
    {"completion_tracker_deduplication", test_completion_tracker_deduplication},
    {"completion_tracker_trims_trailing_spaces", test_completion_tracker_trims_trailing_spaces},
    {"completion_tracker_max_results", test_completion_tracker_max_results},
    {"completion_tracker_delete_before_bounds", test_completion_tracker_delete_before_bounds},
    {"completion_apply_consumes_quoted_suffix", test_completion_apply_consumes_quoted_suffix},
    {"completion_apply_moves_over_existing_suffix",
     test_completion_apply_moves_over_existing_suffix},
    {"completion_hint_suppresses_existing_multiline_suffix",
     test_completion_hint_suppresses_existing_multiline_suffix},
    {"completion_apply_consumes_existing_multiline_suffix",
     test_completion_apply_consumes_existing_multiline_suffix},
    {"completion_spec_round_trip", test_completion_spec_round_trip},
    {"completion_spec_legacy_compatibility", test_completion_spec_legacy_compatibility},
    {"man_page_value_metadata", test_man_page_value_metadata},
    {"rich_completion_runtime", test_rich_completion_runtime},
    {"hints_defer_documentation_fetch", test_hints_defer_documentation_fetch},
    {"tab_path_candidates_and_refresh", test_tab_path_candidates_and_refresh},
    {"hints_defer_dynamic_providers", test_hints_defer_dynamic_providers},
    {"command_context_completion_runtime", test_command_context_completion_runtime},
    {"builtin_docs", test_builtin_docs},
};

int main() {
    // Persistence paths are cached on first use; isolate history and learned documentation.
    namespace fs = std::filesystem;
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path history_dir =
        fs::temp_directory_path() / ("cjsh_completion_history_" + std::to_string(unique_suffix));
    std::error_code ec;
    (void)fs::create_directories(history_dir, ec);
    if (ec || setenv("HOME", history_dir.c_str(), 1) != 0 ||
        setenv("CJSH_HISTORY_FILE", (history_dir / "history.txt").c_str(), 1) != 0) {
        (void)std::fprintf(stderr, "Failed to create isolated completion history\n");
        (void)fs::remove_all(history_dir, ec);
        return 1;
    }

    size_t failures = 0;
    const size_t test_count = sizeof(kTests) / sizeof(kTests[0]);

    for (auto kTest : kTests) {
        if (!kTest.fn()) {
            (void)std::fprintf(stderr, "Test '%s' failed\n", kTest.name);
            failures += 1;
        }
    }

    (void)fs::remove_all(history_dir, ec);

    if (failures > 0) {
        (void)std::fprintf(stderr, "%zu/%zu completion tests failed\n", failures, test_count);
        return 1;
    }

    (void)std::printf("All %zu completion tests passed\n", test_count);
    return 0;
}
