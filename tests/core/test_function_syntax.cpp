/*
  test_function_syntax.cpp

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
#include <vector>

#include "interpreter.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "token_classifier.h"

std::unique_ptr<Shell> g_shell;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        (void)std::fprintf(stderr, "[FAIL] %s\n", message);
        return false;
    }
    return true;
}

bool test_function_keyword_with_parentheses() {
    const std::string definition = "function name() {}";
    ShellScriptInterpreter* interpreter = g_shell->get_shell_script_interpreter();
    if (!expect(interpreter != nullptr, "shell interpreter should be available")) {
        return false;
    }

    const auto syntax_errors = interpreter->validate_function_syntax({definition});
    bool ok = expect(syntax_errors.empty(),
                     "function name() should not include parentheses in validation");

    size_t name_start = 0;
    size_t name_end = 0;
    ok = expect(token_classifier::is_function_definition(definition, name_start, name_end),
                "function name() should be classified as a function definition") &&
         ok;
    ok = expect(definition.substr(name_start, name_end - name_start) == "name",
                "function classifier should return only the function name") &&
         ok;

    ok =
        expect(g_shell->execute(definition) == 0, "function name() should register successfully") &&
        ok;
    ok = expect(interpreter->has_function("name"),
                "registered function should use the name without parentheses") &&
         ok;
    ok = expect(!interpreter->has_function("name()"),
                "parentheses should not be part of the registered function name") &&
         ok;
    return ok;
}

bool test_invalid_keyword_function_name_still_fails_validation() {
    ShellScriptInterpreter* interpreter = g_shell->get_shell_script_interpreter();
    if (!expect(interpreter != nullptr, "shell interpreter should be available")) {
        return false;
    }

    const auto syntax_errors = interpreter->validate_function_syntax({"function bad-name() {}"});
    bool ok = expect(syntax_errors.size() == 1 && syntax_errors.front().error_code == "FUNC002",
                     "invalid characters in function names should still be rejected");

    const auto missing_name_errors = interpreter->validate_function_syntax({"function"});
    ok = expect(
             missing_name_errors.size() == 1 && missing_name_errors.front().error_code == "FUNC001",
             "the function keyword without a name should still be rejected") &&
         ok;
    return ok;
}

bool test_inline_function_validation() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::string> definitions = {
        "function sayhello {echo hello}",
        "function sayhello { echo hello; }",
        "function sayhello() { echo hello; }",
        "function sayhello() {}",
        "sayhello() { echo hello; }",
        "sayhello () { echo hello; }",
        "function sayhello { echo hello; } # trailing comment",
        "function sayhello ( echo hello )",
        "function first { :; }; function second { :; }",
        "function first { :; }; echo function second",
    };
    bool ok = true;
    for (const auto& definition : definitions) {
        ok = expect(interpreter->validate_script_syntax({definition}).empty(),
                    ("inline function should validate: " + definition).c_str()) &&
             ok;
        const auto lines = interpreter->parse_into_lines(definition);
        ok = expect(interpreter->validate_comprehensive_syntax(lines, false, false).empty(),
                    ("status validation should accept: " + definition).c_str()) &&
             ok;
        ok = expect(!interpreter->needs_additional_input(lines),
                    ("closed function should not request more input: " + definition).c_str()) &&
             ok;
    }
    ok = expect(g_shell->execute("function compact {return 7}") == 0 &&
                    g_shell->execute("compact") == 7,
                "compact function should still register and execute") &&
         ok;
    return ok;
}

bool test_function_brace_matching() {
    auto* interpreter = g_shell->get_shell_script_interpreter();
    const std::vector<std::string> complete = {
        "function braces { echo '{'; }",
        "function braces { echo \"}\"; }",
        "function braces { echo \\{; }",
        "function braces { echo \\}; }",
        "function braces { echo ${value:-fallback}; }",
        "function braces { { echo nested; }; }",
    };
    const std::vector<std::string> incomplete = {
        "function braces",
        "function braces {",
        "function braces { echo hello",
        "function braces { echo '}'",
        "function braces { echo \"}\"",
        "function braces { echo \\}",
        "function braces { echo hello # }",
        "function braces { echo ${value:-fallback}",
        "function braces { { echo nested; }",
        "function first { :; }; function second {",
        "function first { :; }; echo ok; function second {",
    };
    bool ok = true;
    for (const auto& definition : complete) {
        ok = expect(interpreter->validate_script_syntax({definition}).empty() &&
                        !interpreter->needs_additional_input({definition}),
                    ("matching function braces should close the body: " + definition).c_str()) &&
             ok;
    }
    for (const auto& definition : incomplete) {
        const auto errors = interpreter->validate_script_syntax({definition});
        ok = expect(errors.size() == 1 && errors.front().error_code == "SYN007" &&
                        interpreter->needs_additional_input({definition}),
                    ("unfinished function should request more input: " + definition).c_str()) &&
             ok;
    }
    ok = expect(interpreter->validate_script_syntax({"function multiline {", "echo hello", "}"})
                    .empty(),
                "multiline function should still close on a later line") &&
         ok;
    std::vector<std::string> nested = {"function outer {", "function inner { echo hello; }"};
    const auto nested_errors = interpreter->validate_script_syntax(nested);
    ok = expect(nested_errors.size() == 1 && nested_errors.front().position.line_number == 1 &&
                    interpreter->needs_additional_input(nested),
                "closed inline function should not close its unfinished outer function") &&
         ok;
    nested.push_back("}");
    ok = expect(interpreter->validate_script_syntax(nested).empty() &&
                    !interpreter->needs_additional_input(nested),
                "inline function inside a multiline function should validate") &&
         ok;
    return ok;
}

bool test_function_definition_lifetime() {
    bool ok = expect(g_shell->execute("self_unset() { unset -f self_unset; return 7; }") == 0 &&
                         g_shell->execute("self_unset") == 7,
                     "an active function body survives unsetting its definition");
    ok = expect(
             g_shell->execute("self_replace() { self_replace() { return 9; }; return 8; }") == 0 &&
                 g_shell->execute("self_replace") == 8 && g_shell->execute("self_replace") == 9,
             "redefinition preserves the active body and replaces subsequent calls") &&
         ok;
    return ok;
}

bool test_function_validation_after_dialect_change() {
    const auto original_dialect = config::shell_dialect();
    config::set_shell_dialect(config::ShellDialect::Cjsh);
    bool ok =
        expect(g_shell->execute("native_body() { [[ yes = yes ]]; }") == 0 &&
                   g_shell->execute("native_body") == 0 && g_shell->execute("native_body") == 0,
               "a valid native function can be invoked repeatedly");
    config::set_shell_dialect(config::ShellDialect::Posix);
    ok = expect(g_shell->execute("native_body") == 2,
                "a previously invoked body is revalidated after changing dialect") &&
         ok;
    cjsh_env::clear_exit_request();
    config::set_shell_dialect(config::ShellDialect::Cjsh);
    ok = expect(g_shell->execute("native_body") == 0,
                "the original body remains usable after restoring its dialect") &&
         ok;
    config::set_shell_dialect(original_dialect);
    return ok;
}

}  // namespace

int main() {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(false);
    // Shell's constructor performs job-control setup; disable it before the
    // constructor can claim the terminal from the test runner.
    config::interactive_mode = false;
    config::force_interactive = false;
    g_shell = std::make_unique<Shell>();
    g_shell->set_interactive_mode(false);

    size_t failures = 0;
    if (!test_function_keyword_with_parentheses()) {
        ++failures;
    }
    if (!test_invalid_keyword_function_name_still_fails_validation()) {
        ++failures;
    }
    if (!test_inline_function_validation()) {
        ++failures;
    }
    if (!test_function_brace_matching()) {
        ++failures;
    }
    if (!test_function_definition_lifetime()) {
        ++failures;
    }
    if (!test_function_validation_after_dialect_change()) {
        ++failures;
    }

    // Match the executable's explicit teardown before process-wide registries
    // are destroyed by static finalization.
    g_shell.reset();

    if (failures != 0) {
        (void)std::fprintf(stderr, "%zu/6 function syntax tests failed\n", failures);
        return 1;
    }

    (void)std::printf("All 6 function syntax tests passed\n");
    return 0;
}
