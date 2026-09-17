/*
  test_validation_tokens.cpp

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

#include <array>
#include <cstdio>
#include <locale>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "error_out.h"
#include "interpreter.h"
#include "shell.h"
#include "shell_env.h"
#include "validation_common.h"

std::unique_ptr<Shell> g_shell;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        (void)std::fprintf(stderr, "[FAIL] %s\n", message);
    }
    return condition;
}

struct CommaWhitespace : std::ctype<char> {
    CommaWhitespace() : std::ctype<char>(classification()) {
    }

    static const mask* classification() {
        static const auto masks = [] {
            std::array<mask, table_size> result{};
            const auto& classic = std::use_facet<std::ctype<char>>(std::locale::classic());
            // classic_table() can be null on musl; query the facet instead.
            for (size_t i = 0; i < result.size(); ++i) {
                const char character = static_cast<char>(i);
                classic.is(&character, &character + 1, &result[i]);
            }
            result[','] |= space;
            result[' '] &= ~space;
            return result;
        }();
        return masks.data();
    }
};

bool test_whitespace_and_locale() {
    using shell_validation::internal::tokenize_and_get_first;
    using shell_validation::internal::tokenize_whitespace;
    const std::locale previous = std::locale::global(std::locale::classic());
    bool ok = true;
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"", {}},
        {" \t\r\n\v\f", {}},
        {"  for\ti\nin\vone\ftwo\r\n", {"for", "i", "in", "one", "two"}},
        {"if; then", {"if;", "then"}},
        {"\"one two\" 'three four'", {"\"one", "two\"", "'three", "four'"}},
        {std::string("a\0b c", 5), {std::string("a\0b", 3), "c"}},
        {"caf\xc3\xa9 next", {"caf\xc3\xa9", "next"}},
    };
    for (const auto& [input, expected] : cases) {
        const auto [tokens, first] = tokenize_and_get_first(input);
        ok = expect(tokens == expected, "validation preserves whitespace token boundaries") && ok;
        ok = expect(first == (expected.empty() ? "" : expected.front()),
                    "validation preserves the first token") &&
             ok;
    }
    std::locale::global(std::locale(std::locale::classic(), new CommaWhitespace));
    ok = expect(tokenize_whitespace(",one two,,three,") ==
                    std::vector<std::string>({"one two", "three"}),
                "validation honors the current C++ locale's whitespace") &&
         ok;
    std::locale::global(previous);
    return ok;
}

bool test_variable_diagnostics() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::string> lines = {
        "export __audit_export='two words'; read -r -p 'prompt words' __audit_read; "
        "declare __audit_declared=value",
        ": \"$__audit_export\" \"$__audit_read\" \"$__audit_declared\" \"$__audit_missing\"",
    };
    auto errors = interpreter->validate_variable_usage(lines);
    bool ok = expect(errors.size() == 1 && errors.front().error_code == "VAR002" &&
                         errors.front().position.line_number == 2 &&
                         errors.front().message.find("__audit_missing") != std::string::npos,
                     "declarations and read operands retain undefined-variable diagnostics");
    interpreter->get_variable_manager().set_environment_variable("__audit_missing", "defined");
    ok = expect(interpreter->validate_variable_usage(lines).empty(),
                "a subsequent validation sees newly defined variables") &&
         ok;
    const auto malformed = interpreter->validate_variable_usage({": \"${__audit_unclosed\""});
    bool found = false;
    for (const auto& error : malformed) {
        found =
            found || (error.error_code == "SYN008" && error.severity == ErrorSeverity::CRITICAL);
    }
    ok = expect(found, "unclosed parameter expansion remains a critical syntax error") && ok;
    return ok;
}

bool test_execution_variable_syntax() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::string> cases = {
        ": literal",
        "value=unused",
        ": $missing",
        ": ${missing:-fallback}",
        ": \"${missing\"",
        ": ${first ${second",
        ": '${literal'",
        ": \\${escaped",
        ": ignored # ${comment",
        ": $((1 + 2)) ${unclosed",
        ": $((1 + ${nested}))",
        ": \"${closed}\" ${open",
        ": \"one\ntwo ${open\"",
    };
    bool ok = true;
    for (const auto& line : cases) {
        const auto complete = interpreter->validate_variable_usage({line});
        const auto syntax = interpreter->validate_variable_usage({line}, false);
        size_t index = 0;
        for (const auto& error : complete) {
            if (error.severity != ErrorSeverity::CRITICAL) {
                continue;
            }
            ok = expect(index < syntax.size(), "execution retains every blocking variable error") &&
                 ok;
            if (index < syntax.size()) {
                const auto& actual = syntax[index];
                ok = expect(actual.error_code == error.error_code &&
                                actual.severity == error.severity &&
                                actual.message == error.message &&
                                actual.position.line_number == error.position.line_number &&
                                actual.suggestion == error.suggestion,
                            "execution preserves variable syntax diagnostics and source lines") &&
                     ok;
            }
            ++index;
        }
        ok = expect(index == syntax.size(), "execution omits advisory variable diagnostics") && ok;
    }
    ok = expect(interpreter->has_syntax_errors({": \"${missing\""}, false),
                "normal execution rejects unclosed parameter expansions") &&
         ok;
    ok = expect(!interpreter->has_syntax_errors({": ${missing:-fallback}"}, false),
                "normal execution accepts default expansion of an unset variable") &&
         ok;
    return ok;
}

bool test_assignment_diagnostics() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::string> defined = {
        "__audit_first=1; __audit_second=2; : $__audit_first $__audit_second",
        "__audit_first=1 __audit_second=2; : $__audit_first $__audit_second",
        "true && __audit_value=1; : $__audit_value",
        "false || __audit_value=1; : $__audit_value",
        "for __audit_item in one; do __audit_value=1; : $__audit_item $__audit_value; done",
        "for __audit_item in one; do\n: $__audit_item\n__audit_value=1\n: $__audit_value\ndone",
        "if __audit_condition=1; then __audit_value=2; : $__audit_condition $__audit_value; fi",
        "if false; then :; elif __audit_value=1; then : $__audit_value; else "
        "__audit_other=2; : $__audit_other; fi",
        "while __audit_value=1; do : $__audit_value; break; done",
        "until __audit_value=1; do : $__audit_value; done",
        "{ __audit_value=1; : $__audit_value; }",
        "(__audit_value=1; : $__audit_value)",
        "__audit_value='two; words'; : $__audit_value",
        "__audit_value==literal; : $__audit_value",
        "__audit_value=~literal; : $__audit_value",
    };
    bool ok = true;
    for (const auto& script : defined) {
        const auto lines = interpreter->parse_into_lines(script);
        ok = expect(interpreter->validate_variable_usage(lines).empty(),
                    ("assignments are recognized throughout command lists: " + script).c_str()) &&
             ok;
    }

    const std::vector<std::string> arguments = {
        "echo __audit_argument=1",
        "echo '__audit_argument=1'",
        "echo \"text; __audit_argument=1\"",
        "echo do __audit_argument=1",
        "echo then __audit_argument=1",
        "echo else __audit_argument=1",
        "echo if __audit_argument=1",
        "echo { __audit_argument=1 }",
        "echo $((1 + 2)) __audit_argument=1",
        "echo ${PATH:-fallback} __audit_argument=1",
        "echo $(printf literal) __audit_argument=1",
        "echo `printf literal` __audit_argument=1",
        "[ __audit_argument=1 = text ]",
        "[[ yes = yes && __audit_argument=1 = text ]]",
        "test __audit_argument=1 = text",
        "for __audit_item in do __audit_argument=1; do : $__audit_item; done",
        ": # __audit_argument=1",
    };
    for (const auto& command : arguments) {
        const auto errors = interpreter->validate_variable_usage(
            interpreter->parse_into_lines(command + "\n: $__audit_argument"));
        ok = expect(errors.size() == 1 && errors.front().error_code == "VAR002" &&
                        errors.front().position.line_number == 2 &&
                        errors.front().message.find("__audit_argument") != std::string::npos,
                    ("assignment-like arguments do not define variables: " + command).c_str()) &&
             ok;
    }

    const auto unused = interpreter->validate_variable_usage({"if __audit_unused=1; then :; fi"});
    ok = expect(unused.size() == 1 && unused.front().error_code == "VAR003",
                "a conditional assignment produces one unused-variable diagnostic") &&
         ok;
    const auto multiline_unused =
        interpreter->validate_variable_usage({"printf '%s' 'first\nsecond';\n__audit_unused=1"});
    ok = expect(multiline_unused.size() == 1 && multiline_unused.front().error_code == "VAR003" &&
                    multiline_unused.front().position.line_number == 3,
                "assignments after multiline quotes retain their source line") &&
         ok;
    return ok;
}

bool test_inline_prime_loop_diagnostics() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    bool ok = true;
    for (const std::string suffix : {"", "\n     "}) {
        const std::string script =
            "for n in $(seq 2 100); do d=2; p=1; while [ $((d*d)) -le $n ]; do "
            "if [ $((n%d)) -eq 0 ]; then p=0; break; fi; d=$((d+1)); done; "
            "[ $p -eq 1 ] && printf '%s ' \"$n" +
            suffix + "\"; done; echo";
        const auto lines = interpreter->parse_into_lines(script);
        ok = expect(!interpreter->needs_additional_input(lines),
                    "the prime loop is complete with either single-line or multiline quotes") &&
             ok;
        ok = expect(interpreter->validate_comprehensive_syntax(lines, false, false).empty(),
                    "the prime loop has no advisory or blocking diagnostics") &&
             ok;
    }
    return ok;
}

bool test_control_validator_filter() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    bool ok = true;
    for (const auto& line : {": ordinary words", ": before select while until if case", "'for' x",
                             "different argument", "casework word"}) {
        ok = expect(interpreter->validate_loop_syntax({line}).empty() &&
                        interpreter->validate_conditional_syntax({line}).empty(),
                    "keyword substrings and quoted words remain ordinary commands") &&
             ok;
    }
    for (const auto& line : {"for;", "for", "select", "while", "until"}) {
        ok = expect(!interpreter->validate_loop_syntax({line}).empty(),
                    "all loop leaders retain incomplete-header diagnostics") &&
             ok;
    }
    for (const auto& line : {"if", "case"}) {
        ok = expect(!interpreter->validate_conditional_syntax({line}).empty(),
                    "conditional leaders retain incomplete-header diagnostics") &&
             ok;
    }
    const std::locale previous =
        std::locale::global(std::locale(std::locale::classic(), new CommaWhitespace));
    ok = expect(!interpreter->validate_loop_syntax({",while,"}).empty() &&
                    !interpreter->validate_conditional_syntax({",if,"}).empty(),
                "keyword filtering preserves custom locale tokenization") &&
         ok;
    std::locale::global(previous);
    return ok;
}

bool test_literal_control_keywords() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::pair<std::string, std::string>> blocks = {
        {"if true; then", "fi"},      {"while false; do", "done"}, {"until true; do", "done"},
        {"for i in one; do", "done"}, {"case x in x)", "esac"},
    };
    bool ok = true;
    for (const auto& [header, closer] : blocks) {
        for (const auto& argument : {closer, "'" + closer + "'", "\"text " + closer + " text\"",
                                     "\\" + closer, "ok # " + closer, "\"; " + closer + "\""}) {
            const std::string unfinished = header + " echo " + argument;
            ok = expect(interpreter->needs_additional_input({unfinished}),
                        ("literal keyword should not close a block: " + unfinished).c_str()) &&
                 ok;
        }
        const std::string closed =
            header + " echo '" + closer + "'" + (closer == "esac" ? ";; " : "; ") + closer;
        ok = expect(!interpreter->needs_additional_input({closed}),
                    ("a real terminator should close the block: " + closed).c_str()) &&
             ok;
    }
    for (const auto& line : {"if echo then", "if echo 'a then b'", "if echo ok # then"}) {
        ok = expect(!interpreter->validate_conditional_syntax({line}).empty(),
                    "a literal then should not satisfy the if header") &&
             ok;
    }
    return ok;
}

}  // namespace

int main() {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(false);
    config::interactive_mode = false;
    config::force_interactive = false;
    g_shell = std::make_unique<Shell>();
    g_shell->set_interactive_mode(false);
    const bool tokens_ok = test_whitespace_and_locale();
    const bool diagnostics_ok = test_variable_diagnostics();
    const bool execution_ok = test_execution_variable_syntax();
    const bool assignments_ok = test_assignment_diagnostics();
    const bool prime_loop_ok = test_inline_prime_loop_diagnostics();
    const bool control_ok = test_control_validator_filter();
    const bool literal_keywords_ok = test_literal_control_keywords();
    g_shell.reset();
    if (tokens_ok && diagnostics_ok && execution_ok && assignments_ok && prime_loop_ok &&
        control_ok && literal_keywords_ok) {
        std::puts("All 7 validation token tests passed");
        return 0;
    }
    (void)std::fprintf(stderr, "%d/7 validation token tests failed\n",
                       !tokens_ok + !diagnostics_ok + !execution_ok + !assignments_ok +
                           !prime_loop_ok + !control_ok + !literal_keywords_ok);
    return 1;
}
