/*
  test_syntax_highlighting.cpp

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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>

extern "C" {
#include "attr.h"
#include "bbcode.h"
#include "common.h"
#include "env.h"
#include "highlight.h"
#include "isocline.h"
}

#include "agent_mode.h"
#include "cjsh_filesystem.h"
#include "cjsh_syntax_highlighter.h"
#include "command_analysis.h"
#include "shell.h"
#include "shell_env.h"
#include "token_constants.h"

std::unique_ptr<Shell> g_shell;

extern "C" void syntax_highlight_bridge(ic_highlight_env_t* henv, const char* input, void* arg) {
    SyntaxHighlighter::highlight(henv, input, arg);
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

static ic_env_t* ensure_env(const char* test_name) {
    ic_env_t* env = ic_get_env();
    if (env == nullptr) {
        log_failure(test_name, "ic_get_env() returned nullptr");
    }
    return env;
}

static void ensure_style_definitions() {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    for (const auto& pair : token_constants::default_styles()) {
        std::string style_name = pair.first;
        if (style_name.rfind("ic-", 0) != 0) {
            style_name.insert(0, "cjsh-");
        }
        ic_style_def(style_name.c_str(), pair.second.c_str());
    }

    initialized = true;
}

static attrbuf_t* highlight_input(const std::string& input, const char* test_name) {
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        return nullptr;
    }

    ensure_style_definitions();
    attrbuf_t* attrs = attrbuf_new(env->mem);
    if (attrs == nullptr) {
        log_failure(test_name, "attrbuf_new() returned nullptr");
        return nullptr;
    }

    highlight(env->mem, env->bbcode, input.c_str(), attrs, syntax_highlight_bridge, nullptr);
    return attrs;
}

static bool expect_style_range(attrbuf_t* attrs, bbcode_t* bbcode, size_t start, size_t length,
                               const char* style, const char* test_name, const char* message) {
    if (length == 0) {
        log_failure(test_name, "expected non-empty highlight range");
        return false;
    }
    attr_t expected = bbcode_style(bbcode, style);
    if (attr_is_none(expected)) {
        log_failure(test_name, "expected style not registered");
        return false;
    }

    for (size_t i = start; i < start + length; ++i) {
        attr_t actual = attrbuf_attr_at(attrs, static_cast<ssize_t>(i));
        if (!attr_is_eq(actual, expected)) {
            log_failure(test_name, message);
            return false;
        }
    }
    return true;
}

static bool expect_not_style_range(attrbuf_t* attrs, bbcode_t* bbcode, size_t start, size_t length,
                                   const char* style, const char* test_name, const char* message) {
    if (length == 0) {
        log_failure(test_name, "expected non-empty highlight range");
        return false;
    }
    attr_t forbidden = bbcode_style(bbcode, style);
    if (attr_is_none(forbidden)) {
        log_failure(test_name, "forbidden style not registered");
        return false;
    }

    for (size_t i = start; i < start + length; ++i) {
        attr_t actual = attrbuf_attr_at(attrs, static_cast<ssize_t>(i));
        if (attr_is_eq(actual, forbidden)) {
            log_failure(test_name, message);
            return false;
        }
    }
    return true;
}

static bool test_variable_assignment_highlighting() {
    const char* test_name = "variable_assignment_highlighting";
    const std::string input = "FOO=42";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, 0, 3, "cjsh-variable", test_name,
                                 "FOO should be highlighted as variable") &&
              expect_style_range(attrs, env->bbcode, 3, 1, "cjsh-operator", test_name,
                                 "= should be highlighted as operator") &&
              expect_style_range(attrs, env->bbcode, 4, 2, "cjsh-number", test_name,
                                 "42 should be highlighted as number");

    attrbuf_free(attrs);
    return ok;
}

static bool test_comment_highlighting() {
    const char* test_name = "comment_highlighting";
    const std::string input = "echo hi # comment";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find('#');
    if (start == std::string::npos) {
        log_failure(test_name, "failed to locate comment marker");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = input.size() - start;
    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-comment", test_name,
                                 "comment range should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_command_substitution_and_variable() {
    const char* test_name = "command_substitution_and_variable";
    const std::string input = "echo $(date) $USER";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t cmd_start = input.find("$(");
    size_t cmd_end = input.find(')', cmd_start);
    if (cmd_start == std::string::npos || cmd_end == std::string::npos || cmd_end < cmd_start) {
        log_failure(test_name, "failed to locate command substitution range");
        attrbuf_free(attrs);
        return false;
    }
    size_t cmd_length = (cmd_end == std::string::npos) ? 0 : (cmd_end - cmd_start + 1);

    size_t var_start = input.find("$USER");
    if (var_start == std::string::npos) {
        log_failure(test_name, "failed to locate $USER token");
        attrbuf_free(attrs);
        return false;
    }
    size_t var_length = std::string("$USER").size();

    bool ok =
        expect_style_range(attrs, env->bbcode, cmd_start, cmd_length, "cjsh-command-substitution",
                           test_name, "$(...) should be highlighted as command substitution") &&
        expect_style_range(attrs, env->bbcode, var_start, var_length, "cjsh-variable", test_name,
                           "$USER should be highlighted as variable");

    attrbuf_free(attrs);
    return ok;
}

static bool test_function_definition_highlighting() {
    const char* test_name = "function_definition_highlighting";
    const std::string input = "myfunc() { echo hi; }";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t brace_pos = input.find('{');
    if (brace_pos == std::string::npos) {
        log_failure(test_name, "failed to locate opening brace");
        attrbuf_free(attrs);
        return false;
    }
    bool ok = expect_style_range(attrs, env->bbcode, 0, 6, "cjsh-function-definition", test_name,
                                 "function name should be highlighted") &&
              expect_style_range(attrs, env->bbcode, 6, 2, "cjsh-function-definition", test_name,
                                 "function parentheses should be highlighted") &&
              expect_style_range(attrs, env->bbcode, brace_pos, 1, "cjsh-operator", test_name,
                                 "opening brace should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_assignment_value_highlighting() {
    const char* test_name = "assignment_value_highlighting";
    const std::string input = "FOO=bar";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, 0, 3, "cjsh-variable", test_name,
                                 "FOO should be highlighted as variable") &&
              expect_style_range(attrs, env->bbcode, 3, 1, "cjsh-operator", test_name,
                                 "= should be highlighted as operator") &&
              expect_style_range(attrs, env->bbcode, 4, 3, "cjsh-assignment-value", test_name,
                                 "bar should be highlighted as assignment value");

    attrbuf_free(attrs);
    return ok;
}

static bool test_arithmetic_substitution_highlighting() {
    const char* test_name = "arithmetic_substitution_highlighting";
    const std::string input = "echo $((1 + 2))";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("$((");
    size_t end = input.rfind("))");
    if (start == std::string::npos || end == std::string::npos || end < start) {
        log_failure(test_name, "failed to locate arithmetic substitution range");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 2;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-arithmetic", test_name,
                                 "arithmetic substitution should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_backtick_command_substitution_highlighting() {
    const char* test_name = "backtick_command_substitution_highlighting";
    const std::string input = "echo `date +%s`";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find('`');
    size_t end = input.rfind('`');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        log_failure(test_name, "failed to locate backtick substitution range");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-command-substitution",
                                 test_name, "backtick command substitution should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_history_expansion_highlighting() {
    const char* test_name = "history_expansion_highlighting";
    const std::string input = "echo !! && echo !$";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t bang_bang = input.find("!!");
    size_t bang_dollar = input.find("!$");
    if (bang_bang == std::string::npos || bang_dollar == std::string::npos) {
        log_failure(test_name, "failed to locate history expansion tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, bang_bang, 2, "cjsh-history-expansion",
                                 test_name, "!! should be highlighted as history expansion") &&
              expect_style_range(attrs, env->bbcode, bang_dollar, 2, "cjsh-history-expansion",
                                 test_name, "!$ should be highlighted as history expansion");

    attrbuf_free(attrs);
    return ok;
}

static bool test_operator_separator_highlighting() {
    const char* test_name = "operator_separator_highlighting";
    const std::string input = "echo ok && echo more || echo last";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t and_pos = input.find("&&");
    size_t or_pos = input.find("||");
    if (and_pos == std::string::npos || or_pos == std::string::npos) {
        log_failure(test_name, "failed to locate command separators");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, and_pos, 2, "cjsh-operator", test_name,
                                 "&& should be highlighted as operator") &&
              expect_style_range(attrs, env->bbcode, or_pos, 2, "cjsh-operator", test_name,
                                 "|| should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_append_redirection_operator_highlighting() {
    const char* test_name = "append_redirection_operator_highlighting";
    const std::string input = "echo hi >> out.txt";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t redir_pos = input.find(">>");
    if (redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate append redirection operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, redir_pos, 2, "cjsh-operator", test_name,
                                 ">> should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_here_string_operator_highlighting() {
    const char* test_name = "here_string_operator_highlighting";
    const std::string input = "cat <<< EOF";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t redir_pos = input.find("<<<");
    if (redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate here-string operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, redir_pos, 3, "cjsh-operator", test_name,
                                 "<<< should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_background_operator_highlighting() {
    const char* test_name = "background_operator_highlighting";
    const std::string input = "sleep 1 & echo done";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t amp_pos = input.find('&');
    if (amp_pos == std::string::npos) {
        log_failure(test_name, "failed to locate background operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, amp_pos, 1, "cjsh-operator", test_name,
                                 "& should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_option_glob_redirection_highlighting() {
    const char* test_name = "option_glob_redirection_highlighting";
    const std::string input = "ls -la *.cpp > out.txt";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t option_pos = input.find("-la");
    size_t glob_pos = input.find("*.cpp");
    size_t redir_pos = input.find("> ");
    if (option_pos == std::string::npos || glob_pos == std::string::npos ||
        redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate option/glob/redirection tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, option_pos, 3, "cjsh-option", test_name,
                                 "-la should be highlighted as option") &&
              expect_style_range(attrs, env->bbcode, glob_pos, 5, "cjsh-glob-pattern", test_name,
                                 "*.cpp should be highlighted as glob pattern") &&
              expect_style_range(attrs, env->bbcode, redir_pos, 1, "cjsh-operator", test_name,
                                 "> should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_keyword_argument_highlighting() {
    const char* test_name = "keyword_argument_highlighting";
    const std::string input = "echo if then fi";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t if_pos = input.find("if");
    size_t then_pos = input.find("then");
    size_t fi_pos = input.rfind("fi");
    if (if_pos == std::string::npos || then_pos == std::string::npos ||
        fi_pos == std::string::npos) {
        log_failure(test_name, "failed to locate keyword tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, if_pos, 2, "cjsh-keyword", test_name,
                                 "if should be highlighted as keyword") &&
              expect_style_range(attrs, env->bbcode, then_pos, 4, "cjsh-keyword", test_name,
                                 "then should be highlighted as keyword") &&
              expect_style_range(attrs, env->bbcode, fi_pos, 2, "cjsh-keyword", test_name,
                                 "fi should be highlighted as keyword");

    attrbuf_free(attrs);
    return ok;
}

static bool test_split_unknown_command_fragment_highlighting() {
    const char* test_name = "split_unknown_command_fragment_highlighting";
    const std::string input = "e cho";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t first_pos = input.find('e');
    size_t second_pos = input.find("cho");
    if (first_pos == std::string::npos || second_pos == std::string::npos) {
        log_failure(test_name, "failed to locate split command fragments");
        attrbuf_free(attrs);
        return false;
    }

    bool ok =
        expect_style_range(attrs, env->bbcode, first_pos, 1, "cjsh-unknown-command", test_name,
                           "first split command fragment should be highlighted as unknown") &&
        expect_style_range(attrs, env->bbcode, second_pos, 3, "cjsh-unknown-command", test_name,
                           "second split command fragment should be highlighted as unknown");

    attrbuf_free(attrs);
    return ok;
}

static bool test_split_unknown_command_fragment_highlighting_with_gap() {
    const char* test_name = "split_unknown_command_fragment_highlighting_with_gap";
    const std::string input = "pri tf";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t first_pos = input.find("pri");
    size_t second_pos = input.find("tf");
    if (first_pos == std::string::npos || second_pos == std::string::npos) {
        log_failure(test_name, "failed to locate split command fragments with gap");
        attrbuf_free(attrs);
        return false;
    }

    bool ok =
        expect_style_range(attrs, env->bbcode, first_pos, 3, "cjsh-unknown-command", test_name,
                           "first split command fragment should be highlighted as unknown") &&
        expect_style_range(attrs, env->bbcode, second_pos, 2, "cjsh-unknown-command", test_name,
                           "second split command fragment should be highlighted as unknown");

    attrbuf_free(attrs);
    return ok;
}

static bool test_split_unknown_command_fragment_highlighting_with_known_second_token() {
    const char* test_name = "split_unknown_command_fragment_highlighting_with_known_second_token";
    if (g_shell == nullptr) {
        log_failure(test_name, "shell instance is not initialized");
        return false;
    }

    auto original_aliases = g_shell->get_aliases();
    auto updated_aliases = original_aliases;
    updated_aliases["code"] = "echo known-code-fragment";
    updated_aliases["opencode"] = "echo merged-command";
    g_shell->set_aliases(updated_aliases);

    const std::string input = "ope code";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        g_shell->set_aliases(original_aliases);
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        g_shell->set_aliases(original_aliases);
        return false;
    }

    size_t first_pos = input.find("ope");
    size_t second_pos = input.find("code");
    if (first_pos == std::string::npos || second_pos == std::string::npos) {
        log_failure(test_name, "failed to locate split command fragments");
        attrbuf_free(attrs);
        g_shell->set_aliases(original_aliases);
        return false;
    }

    bool ok =
        expect_style_range(attrs, env->bbcode, first_pos, 3, "cjsh-unknown-command", test_name,
                           "first split command fragment should be highlighted as unknown") &&
        expect_style_range(
            attrs, env->bbcode, second_pos, 4, "cjsh-unknown-command", test_name,
            "known second fragment should still be highlighted as unknown in a split typo");

    attrbuf_free(attrs);
    g_shell->set_aliases(original_aliases);
    return ok;
}

static bool test_split_command_path_changes_between_highlights() {
    const char* test_name = "split_command_path_changes_between_highlights";
    namespace fs = std::filesystem;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("cjsh_highlight_path_" + std::to_string(suffix));
    const fs::path populated = root / "populated";
    const fs::path empty = root / "empty";
    fs::create_directories(populated);
    fs::create_directories(empty);
    const fs::path executable = populated / "auditXfragment";
    std::ofstream(executable) << "#!/bin/sh\n";
    fs::permissions(executable, fs::perms::owner_read | fs::perms::owner_exec);

    const std::string original_path = cjsh_env::get_shell_variable_value("PATH");
    const std::string input = "audit fragment; audit fragment";
    ic_env_t* env = ensure_env(test_name);
    bool ok = env != nullptr;
    for (const auto& path : {populated, empty, populated}) {
        (void)cjsh_env::set_shell_variable_value("PATH", path.string());
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr || env == nullptr) {
            ok = false;
        } else {
            for (size_t pos = input.find("fragment"); pos != std::string::npos;
                 pos = input.find("fragment", pos + 1)) {
                if (path == populated) {
                    ok = expect_style_range(attrs, env->bbcode, pos, 8, "cjsh-unknown-command",
                                            test_name,
                                            "both split fragments should see PATH candidates") &&
                         ok;
                } else {
                    ok = expect_not_style_range(attrs, env->bbcode, pos, 8, "cjsh-unknown-command",
                                                test_name,
                                                "a new highlight must see the changed PATH") &&
                         ok;
                }
            }
        }
        if (attrs != nullptr) {
            attrbuf_free(attrs);
        }
    }
    (void)cjsh_env::set_shell_variable_value("PATH", original_path);
    cjsh_filesystem::reset_path_hash();
    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

static bool test_redraw_lookup_cache_refresh() {
    const char* test_name = "redraw_lookup_cache_refresh";
    namespace fs = std::filesystem;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("cjsh_redraw_cache_" + std::to_string(suffix));
    fs::create_directories(root);
    const fs::path target = root / "target";
    const std::string token = target.string();
    const std::string input = token + "; echo " + token + " " + token + "; " + token;
    ic_env_t* env = ensure_env(test_name);
    bool ok = env != nullptr;
    // Reuse tokens within a redraw, but observe creation, type changes and deletion
    // on the next callback without a PATH change or explicit invalidation.
    for (int phase = 0; phase < 4; ++phase) {
        if (phase == 1) {
            std::ofstream(target) << "content\n";
        } else if (phase == 2) {
            fs::remove(target);
            fs::create_directory(target);
        } else if (phase == 3) {
            fs::remove(target);
        }
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr || env == nullptr) {
            ok = false;
        } else {
            const char* command_style =
                phase == 0 || phase == 3 ? "cjsh-unknown-command" : "cjsh-system";
            const char* argument_style = phase == 1   ? "cjsh-file-argument"
                                         : phase == 2 ? "cjsh-path-exists"
                                                      : "cjsh-path-not-exists";
            size_t occurrence = 0;
            for (size_t pos = input.find(token); pos != std::string::npos;
                 pos = input.find(token, pos + token.size()), ++occurrence) {
                ok = expect_style_range(
                         attrs, env->bbcode, pos, token.size(),
                         occurrence == 0 || occurrence == 3 ? command_style : argument_style,
                         test_name, "redraw must refresh repeated path lookups") &&
                     ok;
            }
        }
        if (attrs != nullptr) {
            attrbuf_free(attrs);
        }
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

static bool test_unknown_command_argument_not_marked_as_unknown_command() {
    const char* test_name = "unknown_command_argument_not_marked_as_unknown_command";
    const std::string unknown_command = "definitelynotrealcmd";
    const std::string argument = "__cjsh_argument_token__";
    const std::string input = unknown_command + " " + argument;
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t command_pos = input.find(unknown_command);
    size_t argument_pos = input.find(argument);
    if (command_pos == std::string::npos || argument_pos == std::string::npos) {
        log_failure(test_name, "failed to locate command and argument tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, command_pos, unknown_command.size(),
                                 "cjsh-unknown-command", test_name,
                                 "unknown command should be highlighted as unknown") &&
              expect_not_style_range(
                  attrs, env->bbcode, argument_pos, argument.size(), "cjsh-unknown-command",
                  test_name, "ordinary argument should not be highlighted as unknown command");

    attrbuf_free(attrs);
    return ok;
}

static bool test_redirection_target_not_marked_as_unknown_command() {
    const char* test_name = "redirection_target_not_marked_as_unknown_command";
    const std::string input = "echo hello > pipe";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    const size_t command_pos = input.find("echo");
    const size_t redirection_pos = input.find('>');
    const size_t target_pos = input.find("pipe");
    if (command_pos == std::string::npos || redirection_pos == std::string::npos ||
        target_pos == std::string::npos) {
        log_failure(test_name, "failed to locate command, redirection, and target tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok =
        expect_not_style_range(attrs, env->bbcode, command_pos, 4, "cjsh-unknown-command",
                               test_name, "known command should not be highlighted as unknown") &&
        expect_style_range(attrs, env->bbcode, redirection_pos, 1, "cjsh-operator", test_name,
                           "redirection operator should be highlighted") &&
        expect_not_style_range(attrs, env->bbcode, target_pos, 4, "cjsh-unknown-command", test_name,
                               "redirection target should not be highlighted as unknown");

    attrbuf_free(attrs);
    return ok;
}

static bool test_braced_variable_highlighting() {
    const char* test_name = "braced_variable_highlighting";
    const std::string input = "echo ${HOME}";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t var_pos = input.find("${HOME}");
    if (var_pos == std::string::npos) {
        log_failure(test_name, "failed to locate braced variable");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, var_pos, 7, "cjsh-variable", test_name,
                                 "${HOME} should be highlighted as variable");

    attrbuf_free(attrs);
    return ok;
}

static bool test_braced_variable_default_highlighting() {
    const char* test_name = "braced_variable_default_highlighting";
    const std::string input = "echo ${VAR:-default}";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t var_pos = input.find("${VAR:-default}");
    if (var_pos == std::string::npos) {
        log_failure(test_name, "failed to locate braced default variable");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, var_pos, 16, "cjsh-variable", test_name,
                                 "${VAR:-default} should be highlighted as variable");

    attrbuf_free(attrs);
    return ok;
}

static bool test_nested_command_substitution_highlighting() {
    const char* test_name = "nested_command_substitution_highlighting";
    const std::string input = "echo $(echo $(date))";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("$(");
    size_t end = input.rfind(')');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        log_failure(test_name, "failed to locate nested command substitution range");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-command-substitution",
                                 test_name, "nested command substitution should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_history_expansion_modifier_highlighting() {
    const char* test_name = "history_expansion_modifier_highlighting";
    const std::string input = "echo !!:p";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("!!:p");
    if (start == std::string::npos) {
        log_failure(test_name, "failed to locate history expansion with modifier");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, start, 4, "cjsh-history-expansion", test_name,
                                 "!!:p should be highlighted as history expansion");

    attrbuf_free(attrs);
    return ok;
}

static bool test_history_expansion_caret_highlighting() {
    const char* test_name = "history_expansion_caret_highlighting";
    const std::string input = "^old^new^";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    bool ok =
        expect_style_range(attrs, env->bbcode, 0, input.size(), "cjsh-history-expansion", test_name,
                           "caret history expansion should be highlighted as history expansion");

    attrbuf_free(attrs);
    auto original_aliases = g_shell->get_aliases();
    auto updated_aliases = original_aliases;
    updated_aliases[input] = "echo alias";
    g_shell->set_aliases(updated_aliases);
    attrs = highlight_input("  " + input, test_name);
    g_shell->set_aliases(original_aliases);
    if (attrs == nullptr) {
        return false;
    }
    ok = expect_style_range(attrs, env->bbcode, 2, input.size(), "cjsh-builtin", test_name,
                            "indented caret alias should retain command styling") &&
         ok;
    attrbuf_free(attrs);
    return ok;
}

static bool test_compound_redirection_operator_highlighting() {
    const char* test_name = "compound_redirection_operator_highlighting";
    const std::string input = "echo hi 2>&1";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t redir_pos = input.find("2>&1");
    if (redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate compound redirection operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, redir_pos, 4, "cjsh-operator", test_name,
                                 "2>&1 should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_comparison_operator_highlighting() {
    const char* test_name = "comparison_operator_highlighting";
    const std::string input = "test 1 -eq 1";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t op_pos = input.find("-eq");
    if (op_pos == std::string::npos) {
        log_failure(test_name, "failed to locate comparison operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, op_pos, 3, "cjsh-operator", test_name,
                                 "-eq should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_escaped_quote_string_highlighting() {
    const char* test_name = "escaped_quote_string_highlighting";
    const std::string input = "echo \"a\\\"b\"";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t quote_pos = input.find('"');
    if (quote_pos == std::string::npos) {
        log_failure(test_name, "failed to locate quoted string");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, quote_pos, 6, "cjsh-string", test_name,
                                 "quoted string with escape should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_double_quoted_string_highlighting() {
    const char* test_name = "double_quoted_string_highlighting";
    const std::string input = "echo \"hello world\"";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find('"');
    size_t end = input.rfind('"');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        log_failure(test_name, "failed to locate double-quoted string");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-string", test_name,
                                 "double-quoted string should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_single_quoted_string_highlighting() {
    const char* test_name = "single_quoted_string_highlighting";
    const std::string input = "echo 'literal $HOME'";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find('\'');
    size_t end = input.rfind('\'');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        log_failure(test_name, "failed to locate single-quoted string");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-string", test_name,
                                 "single-quoted string should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_nested_quote_string_highlighting() {
    const char* test_name = "nested_quote_string_highlighting";
    const std::string input = "echo \"she said 'hi'\"";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find('"');
    size_t end = input.rfind('"');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        log_failure(test_name, "failed to locate nested-quote string");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-string", test_name,
                                 "nested-quote string should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_bracket_glob_highlighting() {
    const char* test_name = "bracket_glob_highlighting";
    const std::string input = "echo file[0-9].txt";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t glob_pos = input.find("file[0-9].txt");
    if (glob_pos == std::string::npos) {
        log_failure(test_name, "failed to locate bracket glob token");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, glob_pos, 13, "cjsh-glob-pattern", test_name,
                                 "bracket glob should be highlighted as glob pattern");

    attrbuf_free(attrs);
    return ok;
}

static bool test_brace_glob_highlighting() {
    const char* test_name = "brace_glob_highlighting";
    const std::string input = "echo {foo,bar}.txt";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t glob_pos = input.find("{foo,bar}.txt");
    if (glob_pos == std::string::npos) {
        log_failure(test_name, "failed to locate brace glob token");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, glob_pos, 13, "cjsh-glob-pattern", test_name,
                                 "brace glob should be highlighted as glob pattern");

    attrbuf_free(attrs);
    return ok;
}

static bool test_heredoc_operator_highlighting() {
    const char* test_name = "heredoc_operator_highlighting";
    const std::string input = "cat << EOF";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t redir_pos = input.find("<<");
    if (redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate heredoc operator");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, redir_pos, 2, "cjsh-operator", test_name,
                                 "<< should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_heredoc_delimiter_highlighting() {
    const char* test_name = "heredoc_delimiter_highlighting";
    struct Case {
        std::string input;
        std::string opening;
        std::string closing;
    };
    const Case cases[] = {
        {"cat << EOF\nhello\nEOF", "EOF", "EOF"},
        {"cat <<EOF", "EOF", ""},
        {"cat <<'EOF'\nhello\nEOF\necho done", "'EOF'", "EOF"},
        {"cat <<\"END TEXT\"\nhello\nEND TEXT", "\"END TEXT\"", "END TEXT"},
        {"cat <<E\"O\"F\nhello\nEOF", "E\"O\"F", "EOF"},
        {"cat <<\\EOF\nhello\nEOF", "\\EOF", "EOF"},
        {"cat <<-EOF\n\thello\n\tEOF", "EOF", "EOF"},
        {"cat <<EOF; echo ready\nhello\nEOF", "EOF", "EOF"},
        {"cat <<$END\nhello\n$END", "$END", "$END"},
        {"cat <<'#END'\nhello\n#END", "'#END'", "#END"},
        {"cat <<EOF\n' \" # <<NOT_A_DOC\nEOF", "EOF", "EOF"},
        {"cat <<EOF\nEOF extra\n EOF\nEOF", "EOF", "EOF"},
        {"cat <<EOF\nunfinished body", "EOF", ""},
        {"f() { cat <<EOF\nhello\nEOF\n}", "EOF", "EOF"},
        {"cat <<EOF \\\n; echo ready\nhello\nEOF", "EOF", "EOF"},
    };
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        return false;
    }
    bool ok = true;
    for (const auto& test : cases) {
        attrbuf_t* attrs = highlight_input(test.input, test_name);
        if (attrs == nullptr) {
            return false;
        }
        ok &= expect_style_range(attrs, env->bbcode, test.input.find(test.opening),
                                 test.opening.size(), "cjsh-heredoc-delimiter", test_name,
                                 "opening marker should use the heredoc delimiter style");
        if (!test.closing.empty()) {
            const bool closing_ok =
                expect_style_range(attrs, env->bbcode, test.input.rfind(test.closing),
                                   test.closing.size(), "cjsh-heredoc-delimiter", test_name,
                                   "closing marker should use the heredoc delimiter style");
            if (!closing_ok) {
                log_failure(test_name, test.input.c_str());
            }
            ok &= closing_ok;
        }
        attrbuf_free(attrs);
    }
    return ok;
}

static bool test_heredoc_delimiter_false_positives() {
    const char* test_name = "heredoc_delimiter_false_positives";
    const std::string inputs[] = {
        "echo '<<EOF'\nEOF",
        "echo \"<<EOF\"\nEOF",
        "echo ok # <<EOF\nEOF",
        "cat <<<EOF\nEOF",
        "echo $((1 << EOF))\nEOF",
        "((1 << EOF))\nEOF",
        "echo \\<\\<EOF\nEOF",
        "cat <<\nEOF",
        "cat << # EOF\nEOF",
        "cat <<'unfinished",
        "echo \"multiline\n<<EOF\"\nEOF",
    };
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        return false;
    }
    bool ok = true;
    for (const auto& input : inputs) {
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr) {
            return false;
        }
        ok &= expect_not_style_range(attrs, env->bbcode, 0, input.size(), "cjsh-heredoc-delimiter",
                                     test_name,
                                     "non-heredoc text must not receive delimiter styling");
        attrbuf_free(attrs);
    }
    return ok;
}

static bool test_heredoc_body_highlighting() {
    const char* test_name = "heredoc_body_highlighting";
    const std::string inputs[] = {
        "cat <<EOF\nEOF extra\n EOF\n\tEOF\n' # <<FAKE\nEOF\necho done",
        "cat <<-EOF\n EOF\n\tEOF extra\n' # <<FAKE\n\tEOF\necho done",
        "cat <<'EOF'\n$HOME $(date) !!\nEOF\necho done",
    };
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        return false;
    }
    bool ok = true;
    for (const auto& input : inputs) {
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr) {
            return false;
        }
        const size_t body_start = input.find('\n') + 1;
        const size_t closing = input.rfind("EOF");
        const size_t body_end = input.rfind('\n', closing) + 1;
        ok &= expect_style_range(attrs, env->bbcode, body_start, body_end - body_start,
                                 "cjsh-string", test_name,
                                 "body text and nonmatching markers must not be shell commands");
        ok &= expect_style_range(attrs, env->bbcode, input.rfind("echo"), 4, "cjsh-builtin",
                                 test_name, "normal highlighting should resume after the heredoc");
        attrbuf_free(attrs);
    }
    return ok;
}

static bool test_multiple_heredoc_delimiters() {
    const char* test_name = "multiple_heredoc_delimiters";
    const std::string input =
        "cat <<FIRST <<-SECOND\nSECOND\nFIRST\n\tSECOND\ncat <<THIRD\nFIRST\nTHIRD";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }
    bool ok = true;
    for (const std::string marker : {"FIRST", "SECOND", "THIRD"}) {
        const size_t opening = input.find(marker);
        const size_t closing =
            marker == "FIRST" ? input.find("FIRST", opening + 1) : input.rfind(marker);
        ok &=
            expect_style_range(attrs, env->bbcode, opening, marker.size(), "cjsh-heredoc-delimiter",
                               test_name, "each opening marker should be highlighted");
        ok &=
            expect_style_range(attrs, env->bbcode, closing, marker.size(), "cjsh-heredoc-delimiter",
                               test_name, "closing markers should follow declaration order");
    }
    ok &= expect_not_style_range(attrs, env->bbcode, input.find("\nSECOND") + 1, 6,
                                 "cjsh-heredoc-delimiter", test_name,
                                 "a later delimiter inside an earlier body is ordinary text");
    ok &= expect_not_style_range(attrs, env->bbcode, input.rfind("FIRST"), 5,
                                 "cjsh-heredoc-delimiter", test_name,
                                 "a previous delimiter inside a later body is ordinary text");
    attrbuf_free(attrs);
    return ok;
}

static bool test_nested_arithmetic_substitution_highlighting() {
    const char* test_name = "nested_arithmetic_substitution_highlighting";
    const std::string input = "echo $((1 + $(echo 2)))";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("$((");
    size_t end = input.rfind("))");
    if (start == std::string::npos || end == std::string::npos || end < start) {
        log_failure(test_name, "failed to locate nested arithmetic substitution");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 2;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-arithmetic", test_name,
                                 "nested arithmetic substitution should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_command_substitution_with_quotes_highlighting() {
    const char* test_name = "command_substitution_with_quotes_highlighting";
    const std::string input = "echo $(printf \"(x)\")";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("$(");
    size_t end = input.rfind(')');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        log_failure(test_name, "failed to locate command substitution with quotes");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 1;

    bool ok =
        expect_style_range(attrs, env->bbcode, start, length, "cjsh-command-substitution",
                           test_name, "command substitution with quotes should be highlighted");

    attrbuf_free(attrs);
    return ok;
}

static bool test_braced_variable_index_highlighting() {
    const char* test_name = "braced_variable_index_highlighting";
    const std::string input = "echo ${arr[0]}";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t var_pos = input.find("${arr[0]}");
    if (var_pos == std::string::npos) {
        log_failure(test_name, "failed to locate braced variable index");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, var_pos, 9, "cjsh-variable", test_name,
                                 "${arr[0]} should be highlighted as variable");

    attrbuf_free(attrs);
    return ok;
}

static bool test_assignment_value_quoted_string_highlighting() {
    const char* test_name = "assignment_value_quoted_string_highlighting";
    const std::string input = "FOO=\"bar\"";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, 0, 3, "cjsh-variable", test_name,
                                 "FOO should be highlighted as variable") &&
              expect_style_range(attrs, env->bbcode, 3, 1, "cjsh-operator", test_name,
                                 "= should be highlighted as operator") &&
              expect_style_range(attrs, env->bbcode, 4, 5, "cjsh-string", test_name,
                                 "quoted assignment value should be highlighted as string");

    attrbuf_free(attrs);
    return ok;
}

static bool test_parameter_expansion_operator_highlighting() {
    const char* test_name = "parameter_expansion_operator_highlighting";
    const std::string input = "echo ${VAR:=42}";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t var_pos = input.find("${VAR:=42}");
    if (var_pos == std::string::npos) {
        log_failure(test_name, "failed to locate parameter expansion");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, var_pos, 10, "cjsh-variable", test_name,
                                 "${VAR:=42} should be highlighted as variable");

    attrbuf_free(attrs);
    return ok;
}

static bool test_compound_redirection_close_highlighting() {
    const char* test_name = "compound_redirection_close_highlighting";
    const std::string input = "echo hi 2>&-";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t redir_pos = input.find("2>&-");
    if (redir_pos == std::string::npos) {
        log_failure(test_name, "failed to locate compound redirection close");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, redir_pos, 4, "cjsh-operator", test_name,
                                 "2>&- should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_arithmetic_parens_highlighting() {
    const char* test_name = "arithmetic_parens_highlighting";
    const std::string input = "echo ((1+2))";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t start = input.find("((");
    size_t end = input.rfind("))");
    if (start == std::string::npos || end == std::string::npos || end < start) {
        log_failure(test_name, "failed to locate arithmetic parens range");
        attrbuf_free(attrs);
        return false;
    }
    size_t length = end - start + 2;

    bool ok = expect_style_range(attrs, env->bbcode, start, length, "cjsh-arithmetic", test_name,
                                 "((1+2)) should be highlighted as arithmetic");

    attrbuf_free(attrs);
    return ok;
}

static bool test_subshell_group_highlighting() {
    const char* test_name = "subshell_group_highlighting";
    const std::string input = "( echo $SHLVL )";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }

    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    size_t open_paren = input.find('(');
    size_t echo_pos = input.find("echo");
    size_t close_paren = input.rfind(')');
    if (open_paren == std::string::npos || echo_pos == std::string::npos ||
        close_paren == std::string::npos) {
        log_failure(test_name, "failed to locate subshell tokens");
        attrbuf_free(attrs);
        return false;
    }

    bool ok = expect_style_range(attrs, env->bbcode, open_paren, 1, "cjsh-operator", test_name,
                                 "opening parenthesis should be highlighted as operator") &&
              expect_style_range(attrs, env->bbcode, echo_pos, 4, "cjsh-builtin", test_name,
                                 "nested command should be highlighted as builtin") &&
              expect_style_range(attrs, env->bbcode, close_paren, 1, "cjsh-operator", test_name,
                                 "closing parenthesis should be highlighted as operator");

    attrbuf_free(attrs);
    return ok;
}

static bool test_subshell_tokens_known_to_validator() {
    const char* test_name = "subshell_tokens_known_to_validator";

    if (g_shell == nullptr) {
        log_failure(test_name, "shell instance is not initialized");
        return false;
    }

    const std::unordered_set<std::string> available_commands = g_shell->get_available_commands();
    bool open_paren_known =
        command_analysis::is_known_command_token("(", 0, g_shell.get(), available_commands);
    bool close_paren_known =
        command_analysis::is_known_command_token(")", 0, g_shell.get(), available_commands);

    EXPECT_TRUE(open_paren_known, test_name, "'(' should be treated as a known shell token");
    EXPECT_TRUE(close_paren_known, test_name, "')' should be treated as a known shell token");

    return true;
}

static bool test_c_style_for_header_command_boundary() {
    const char* test_name = "c_style_for_header_command_boundary";
    const std::string inputs[] = {"for ((i=0; i<3; i++)); do echo hi; done",
                                  "for ((;;)); do echo hi; done"};

    return std::all_of(std::begin(inputs), std::end(inputs), [&](const auto& input) {
        const std::string sanitized = command_analysis::sanitize_input_for_analysis(input);
        size_t first_semicolon = input.find(';');
        size_t expected_end = input.find("; do");
        if (first_semicolon == std::string::npos || expected_end == std::string::npos) {
            log_failure(test_name, "failed to locate expected semicolon positions");
            return false;
        }

        size_t cmd_end = command_analysis::find_command_end(sanitized, 0);
        EXPECT_TRUE(cmd_end != first_semicolon, test_name,
                    "command boundary should ignore semicolons inside ((...))");
        EXPECT_TRUE(cmd_end == expected_end, test_name,
                    "command boundary should stop at '; do' in C-style for header");

        auto separator = command_analysis::scan_command_separator(sanitized, cmd_end);
        EXPECT_TRUE(separator.length == 1 && separator.is_operator, test_name,
                    "'; do' separator should be recognized as an operator");
        return true;
    });
}

static bool test_existing_file_argument_highlighting() {
    const char* test_name = "existing_file_argument_highlighting";
    const std::string filename = ".cjsh_file_argument_highlight_test";
    const std::filesystem::path file_path = std::filesystem::current_path() / filename;

    std::ofstream output(file_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        log_failure(test_name, "failed to create temporary test file");
        return false;
    }
    output << "test";
    output.close();

    const std::string inputs[] = {"cat " + filename, "cat ./" + filename};
    bool ok = true;
    for (const auto& input : inputs) {
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr) {
            ok = false;
            break;
        }

        ic_env_t* env = ensure_env(test_name);
        if (env == nullptr) {
            attrbuf_free(attrs);
            ok = false;
            break;
        }

        const size_t argument_pos = input.find(' ') + 1;
        const size_t argument_length = input.size() - argument_pos;
        ok = expect_style_range(attrs, env->bbcode, argument_pos, argument_length,
                                "cjsh-file-argument", test_name,
                                "existing file argument should be highlighted as file argument");
        attrbuf_free(attrs);
        if (!ok) {
            break;
        }
    }

    std::error_code remove_error;
    (void)std::filesystem::remove(file_path, remove_error);

    return ok;
}

static bool test_existing_directory_argument_highlighting() {
    const char* test_name = "existing_directory_argument_highlighting";
    const std::string dirname = ".cjsh_directory_argument_highlight_test";
    const std::filesystem::path directory_path = std::filesystem::current_path() / dirname;

    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(directory_path, filesystem_error) || filesystem_error) {
        log_failure(test_name, "failed to create temporary test directory");
        return false;
    }

    const std::string inputs[] = {"cat " + dirname, "cat ./" + dirname, "cd " + dirname, "cd ~",
                                  "cd -"};
    bool ok = true;
    for (const auto& input : inputs) {
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr) {
            ok = false;
            break;
        }

        ic_env_t* env = ensure_env(test_name);
        if (env == nullptr) {
            attrbuf_free(attrs);
            ok = false;
            break;
        }

        const size_t argument_pos = input.find(' ') + 1;
        const size_t argument_length = input.size() - argument_pos;
        ok = expect_style_range(attrs, env->bbcode, argument_pos, argument_length,
                                "cjsh-path-exists", test_name,
                                "existing directory should be highlighted as a valid path") &&
             expect_not_style_range(attrs, env->bbcode, argument_pos, argument_length,
                                    "cjsh-file-argument", test_name,
                                    "existing directory should not be highlighted as a file");
        attrbuf_free(attrs);
        if (!ok) {
            break;
        }
    }

    (void)std::filesystem::remove(directory_path, filesystem_error);
    return ok;
}

static bool test_agent_trigger_prefix_highlighting() {
    const char* test_name = "agent_trigger_prefix_highlighting";
    cjsh_env::set_startup_active(true);
    const bool configured = agent_mode::command({"agent-mode", "reset"}) == 0 &&
                            agent_mode::command({"agent-mode", "set", "--command", "true",
                                                 "--trigger-prefix", "ai:"}) == 0 &&
                            agent_mode::command({"agent-mode", "set", "--command", "true",
                                                 "--trigger-prefix", "ai: "}) == 0;
    cjsh_env::set_startup_active(false);
    EXPECT_TRUE(configured, test_name, "agent trigger prefixes should configure successfully");

    const std::string input = "ai: list files # this is natural language";
    attrbuf_t* attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }
    ic_env_t* env = ensure_env(test_name);
    if (env == nullptr) {
        attrbuf_free(attrs);
        return false;
    }

    const size_t prefix_length = std::string("ai: ").size();
    bool ok =
        expect_style_range(attrs, env->bbcode, 0, prefix_length, "cjsh-agent-prefix", test_name,
                           "the longest matching trigger should use the agent-prefix style") &&
        expect_style_range(attrs, env->bbcode, prefix_length, input.size() - prefix_length,
                           "cjsh-agent-request", test_name,
                           "the complete natural-language request should use one style") &&
        expect_not_style_range(attrs, env->bbcode, input.find('#'), input.size() - input.find('#'),
                               "cjsh-comment", test_name,
                               "shell syntax rules should not run inside an agent request");
    attrbuf_free(attrs);

    const std::string prefix_only = "ai: ";
    attrs = highlight_input(prefix_only, test_name);
    if (attrs == nullptr) {
        return false;
    }
    ok = ok && expect_style_range(attrs, env->bbcode, 0, prefix_only.size(), "cjsh-agent-prefix",
                                  test_name,
                                  "a prefix-only request should retain the agent-prefix style");
    attrbuf_free(attrs);

    const std::string nonmatching = "echo ai: request";
    attrs = highlight_input(nonmatching, test_name);
    if (attrs == nullptr) {
        return false;
    }
    ok = ok && expect_not_style_range(attrs, env->bbcode, 0, nonmatching.size(),
                                      "cjsh-agent-request", test_name,
                                      "agent prefixes should only match at the start of input");
    attrbuf_free(attrs);

    cjsh_env::set_startup_active(true);
    const bool disabled = agent_mode::command({"agent-mode", "off"}) == 0;
    cjsh_env::set_startup_active(false);
    EXPECT_TRUE(disabled, test_name, "agent mode should disable successfully");
    attrs = highlight_input(input, test_name);
    if (attrs == nullptr) {
        return false;
    }
    ok = ok && expect_not_style_range(attrs, env->bbcode, 0, prefix_length, "cjsh-agent-prefix",
                                      test_name,
                                      "disabled agent mode should use normal shell highlighting");
    attrbuf_free(attrs);

    cjsh_env::set_startup_active(true);
    (void)agent_mode::command({"agent-mode", "reset"});
    cjsh_env::set_startup_active(false);
    return ok;
}

static bool test_command_membership_changes_between_redraws() {
    const char* test_name = "command_membership_changes_between_redraws";
    const std::string name = "__cjsh_redraw_command";
    auto check = [&](const std::string& input, size_t start, const char* style) {
        attrbuf_t* attrs = highlight_input(input, test_name);
        if (attrs == nullptr) {
            return false;
        }
        const bool result =
            expect_style_range(attrs, ic_get_env()->bbcode, start, name.size(), style, test_name,
                               "command style must reflect current bindings");
        attrbuf_free(attrs);
        return result;
    };
    bool ok = check(name, 0, "cjsh-unknown-command");
    // Direct map edits are supported by the shell; no cache invalidation hook is required.
    g_shell->get_aliases()[name] = "echo alias";
    ok = check(name, 0, "cjsh-builtin") && ok;
    ok = check("sudo " + name, 5, "cjsh-builtin") && ok;
    g_shell->get_aliases().erase(name);
    ok = check(name, 0, "cjsh-unknown-command") && ok;
    ok = (g_shell->execute(name + "() { :; }") == 0) && ok;
    ok = check(name, 0, "cjsh-builtin") && ok;
    ok = check("sudo " + name, 5, "cjsh-builtin") && ok;
    g_shell->get_aliases()[name] = "echo alias over function";
    g_shell->get_aliases().erase(name);
    ok = check(name, 0, "cjsh-builtin") && ok;
    return ok;
}

using test_fn_t = bool (*)();

using test_case_t = struct test_case_s {
    const char* name;
    test_fn_t fn;
};

static const test_case_t kTests[] = {
    {"command_membership_changes_between_redraws", test_command_membership_changes_between_redraws},
    {"variable_assignment_highlighting", test_variable_assignment_highlighting},
    {"comment_highlighting", test_comment_highlighting},
    {"command_substitution_and_variable", test_command_substitution_and_variable},
    {"function_definition_highlighting", test_function_definition_highlighting},
    {"assignment_value_highlighting", test_assignment_value_highlighting},
    {"arithmetic_substitution_highlighting", test_arithmetic_substitution_highlighting},
    {"backtick_command_substitution_highlighting", test_backtick_command_substitution_highlighting},
    {"history_expansion_highlighting", test_history_expansion_highlighting},
    {"operator_separator_highlighting", test_operator_separator_highlighting},
    {"append_redirection_operator_highlighting", test_append_redirection_operator_highlighting},
    {"here_string_operator_highlighting", test_here_string_operator_highlighting},
    {"background_operator_highlighting", test_background_operator_highlighting},
    {"option_glob_redirection_highlighting", test_option_glob_redirection_highlighting},
    {"keyword_argument_highlighting", test_keyword_argument_highlighting},
    {"split_unknown_command_fragment_highlighting",
     test_split_unknown_command_fragment_highlighting},
    {"split_unknown_command_fragment_highlighting_with_gap",
     test_split_unknown_command_fragment_highlighting_with_gap},
    {"split_command_path_changes_between_highlights",
     test_split_command_path_changes_between_highlights},
    {"redraw_lookup_cache_refresh", test_redraw_lookup_cache_refresh},
    {"split_unknown_command_fragment_highlighting_with_known_second_token",
     test_split_unknown_command_fragment_highlighting_with_known_second_token},
    {"unknown_command_argument_not_marked_as_unknown_command",
     test_unknown_command_argument_not_marked_as_unknown_command},
    {"redirection_target_not_marked_as_unknown_command",
     test_redirection_target_not_marked_as_unknown_command},
    {"braced_variable_highlighting", test_braced_variable_highlighting},
    {"braced_variable_default_highlighting", test_braced_variable_default_highlighting},
    {"nested_command_substitution_highlighting", test_nested_command_substitution_highlighting},
    {"history_expansion_modifier_highlighting", test_history_expansion_modifier_highlighting},
    {"history_expansion_caret_highlighting", test_history_expansion_caret_highlighting},
    {"compound_redirection_operator_highlighting", test_compound_redirection_operator_highlighting},
    {"comparison_operator_highlighting", test_comparison_operator_highlighting},
    {"escaped_quote_string_highlighting", test_escaped_quote_string_highlighting},
    {"double_quoted_string_highlighting", test_double_quoted_string_highlighting},
    {"single_quoted_string_highlighting", test_single_quoted_string_highlighting},
    {"nested_quote_string_highlighting", test_nested_quote_string_highlighting},
    {"bracket_glob_highlighting", test_bracket_glob_highlighting},
    {"brace_glob_highlighting", test_brace_glob_highlighting},
    {"heredoc_operator_highlighting", test_heredoc_operator_highlighting},
    {"heredoc_delimiter_highlighting", test_heredoc_delimiter_highlighting},
    {"heredoc_delimiter_false_positives", test_heredoc_delimiter_false_positives},
    {"multiple_heredoc_delimiters", test_multiple_heredoc_delimiters},
    {"heredoc_body_highlighting", test_heredoc_body_highlighting},
    {"nested_arithmetic_substitution_highlighting",
     test_nested_arithmetic_substitution_highlighting},
    {"command_substitution_with_quotes_highlighting",
     test_command_substitution_with_quotes_highlighting},
    {"braced_variable_index_highlighting", test_braced_variable_index_highlighting},
    {"assignment_value_quoted_string_highlighting",
     test_assignment_value_quoted_string_highlighting},
    {"parameter_expansion_operator_highlighting", test_parameter_expansion_operator_highlighting},
    {"compound_redirection_close_highlighting", test_compound_redirection_close_highlighting},
    {"arithmetic_parens_highlighting", test_arithmetic_parens_highlighting},
    {"subshell_group_highlighting", test_subshell_group_highlighting},
    {"subshell_tokens_known_to_validator", test_subshell_tokens_known_to_validator},
    {"c_style_for_header_command_boundary", test_c_style_for_header_command_boundary},
    {"existing_file_argument_highlighting", test_existing_file_argument_highlighting},
    {"existing_directory_argument_highlighting", test_existing_directory_argument_highlighting},
    {"agent_trigger_prefix_highlighting", test_agent_trigger_prefix_highlighting},
};

int main() {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(false);
    g_shell = std::make_unique<Shell>();
    g_shell->set_interactive_mode(false);
    config::history_expansion_enabled = true;

    size_t failures = 0;
    const size_t test_count = sizeof(kTests) / sizeof(kTests[0]);

    for (auto kTest : kTests) {
        if (!kTest.fn()) {
            (void)std::fprintf(stderr, "Test '%s' failed\n", kTest.name);
            failures += 1;
        }
    }

    // Match the executable's explicit teardown before process-wide registries
    // are destroyed by static finalization.
    g_shell.reset();

    if (failures > 0) {
        (void)std::fprintf(stderr, "%zu/%zu syntax highlighting tests failed\n", failures,
                           test_count);
        return 1;
    }

    (void)std::printf("All %zu syntax highlighting tests passed\n", test_count);
    return 0;
}
