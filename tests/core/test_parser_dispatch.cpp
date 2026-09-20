/*
  test_parser_dispatch.cpp

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
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "builtin_help.h"
#include "cjsh_filesystem.h"
#include "interpreter_utils.h"
#include "parser.h"
#include "shell.h"
#include "shell_env.h"
#include "variable_expander.h"

std::unique_ptr<Shell> g_shell;

namespace {
size_t checks = 0;
size_t failures = 0;

void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        (void)std::fprintf(stderr, "[FAIL] %s\n", message);
    }
}

void test_logical_commands(Parser& parser) {
    // Without an active logical operator, preserve the original input verbatim.
    for (const std::string input :
         {"", " \t\n", ": word", " : one; : two ", ": a | b", ": 'a && b'", ": \"a || b\"",
          "if true; then :; fi", ": \"unterminated"}) {
        const auto commands = parser.parse_logical_commands(input);
        expect(input.empty()
                   ? commands.empty()
                   : commands.size() == 1 && commands[0].command == input && commands[0].op.empty(),
               "logical parsing preserves unsplit text and whitespace");
    }
    const auto commands = parser.parse_logical_commands(": one && : two || : three");
    expect(commands.size() == 3 && commands[0].command == ": one " && commands[0].op == "&&" &&
               commands[1].command == " : two " && commands[1].op == "||" &&
               commands[2].command == " : three" && commands[2].op.empty(),
           "logical parsing retains operator ordering and operand whitespace");
    const auto nested = parser.parse_logical_commands("{ : one && : two; } || : three");
    expect(nested.size() == 2 && nested[0].command == "{ : one && : two; } " &&
               nested[0].op == "||" && nested[1].command == " : three",
           "logical operators inside command groups do not split the outer command");
}

void test_semicolon_commands(Parser& parser) {
    struct Case {
        std::string input;
        bool newlines;
        std::vector<std::string> expected;
    };
    const std::vector<Case> cases = {
        {"", false, {}},
        {" \t\r\n", false, {}},
        {" \t: word\r\n", false, {": word"}},
        {": \"one two\"", false, {": \"one two\""}},
        {": one\n: two", false, {": one\n: two"}},
        {": one\n: two", true, {": one", ": two"}},
        {": \"one\ntwo\"", true, {": \"one\ntwo\""}},
        {"; : one;; : two;", false, {": one", ": two"}},
        {": \"one;two\"; : three", false, {": \"one;two\"", ": three"}},
        {": one\\;two; : three", false, {": one\\;two", ": three"}},
        {"{ : one; : two; }; : three", false, {"{ : one; : two; }", ": three"}},
        {"if true; then :; fi; : next", false, {"if true; then :; fi", ": next"}},
        {std::string(8192, 'x'), false, {std::string(8192, 'x')}},
        {std::string(": a\0b", 6), false, {std::string(": a\0b", 6)}},
    };
    for (const auto& test : cases) {
        expect(parser.parse_semicolon_commands(test.input, test.newlines) == test.expected,
               "semicolon parsing preserves quoting, groups, escapes, and newline policy");
    }
}

void test_comments() {
    using shell_script_interpreter::detail::strip_inline_comment;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"", ""},
        {" : word \t", " : word \t"},
        {": 'one two'", ": 'one two'"},
        {": value # comment", ": value "},
        {": '#' \"#\" # comment", ": '#' \"#\" "},
        {": $# ${#value} ${value#prefix} # comment", ": $# ${#value} ${value#prefix} "},
        {std::string(8192, 'x'), std::string(8192, 'x')},
        {std::string("a\0b", 3), std::string("a\0b", 3)},
    };
    for (const auto& [input, expected] : cases) {
        expect(strip_inline_comment(input) == expected,
               "comment stripping preserves literals, parameters, and arbitrary bytes");
    }
}

void test_ampersand_commands() {
    using shell_script_interpreter::detail::split_ampersand;
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"", {}},
        {" \t\r\n", {}},
        {" \t: word\r\n", {": word"}},
        {": 'one two'", {": 'one two'"}},
        {": $((1 + 2))", {": $((1 + 2))"}},
        {std::string(8192, 'x'), {std::string(8192, 'x')}},
        {std::string("a\0b", 3), {std::string("a\0b", 3)}},
        {": one & : two", {": one &", ": two"}},
        {": 'one&two'", {": 'one&two'"}},
        {": one\\&two", {": one\\&two"}},
        {": one && : two", {": one && : two"}},
        {": >&2", {": >&2"}},
        {": &>out", {": &>out"}},
        {": $((1 & 2))", {": $((1 & 2))"}},
        {"[[ one & two ]]", {"[[ one & two ]]"}},
    };
    for (const auto& [input, expected] : cases) {
        expect(split_ampersand(input) == expected,
               "ampersand splitting preserves literals, arithmetic, redirections and background "
               "lists");
    }
}

void test_help() {
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const std::vector<std::string> dynamic_help = {"dynamic help", "second line"};
    expect(!builtin_handle_help({":"}, {"literal help"}) && output.str().empty(),
           "ordinary builtin calls do not print inline help");
    expect(!builtin_handle_help({":", "operand", "--help"}, {"literal help"}),
           "first-argument help ignores later operands");
    expect(builtin_handle_help({":", "--help"}, {"literal help", "second line"}) &&
               output.str() == "literal help\nsecond line\n",
           "inline help prints all lines");
    output.str("");
    expect(builtin_handle_help({":", "operand", "--help"}, dynamic_help,
                               BuiltinHelpScanMode::AnyArgument) &&
               output.str() == "dynamic help\nsecond line\n",
           "owning help supports scanning all arguments");
    output.str("");
    expect(builtin_handle_help({":", "operand", "--help"}, {std::string("temporary ") + "help"},
                               BuiltinHelpScanMode::AnyArgument) &&
               output.str() == "temporary help\n",
           "inline help can borrow a temporary string for the duration of the call");
    output.str("");
    cjsh_env::set_startup_active(true);
    expect(builtin_handle_help_with_startup_guard({":", "--help"}, {"literal help"}) &&
               builtin_handle_help_with_startup_guard({":", "--help"}, dynamic_help) &&
               output.str().empty(),
           "both help representations suppress output during startup");
    cjsh_env::set_startup_active(false);
    expect(builtin_handle_help_with_startup_guard({":", "--help"}, {"literal help"}) &&
               output.str() == "literal help\n",
           "guarded inline help prints after startup");
    std::cout.rdbuf(previous);
}

void test_execution() {
    expect(g_shell->execute("dispatch_value=initial\n"
                            "false && dispatch_value=wrong\n"
                            "true || dispatch_value=wrong\n"
                            "true && dispatch_value='one;two'\n"
                            "dispatch_value=\"${dispatch_value#one;}\" # comment\n") == 0 &&
               cjsh_env::get_shell_variable_value("dispatch_value") == "two",
           "execution retains short circuiting, assignments, quotes, and parameter expansion");
    expect(g_shell->execute("dispatch_fn() { dispatch_value=$1; }\n"
                            "dispatch_fn first\ndispatch_fn second\n") == 0 &&
               cjsh_env::get_shell_variable_value("dispatch_value") == "second",
           "repeated function calls see current positional parameters");
    expect(g_shell->execute(": ignored --help") == 0 &&
               g_shell->execute("true ignored --help") == 0 &&
               g_shell->execute("false ignored --help") == 1,
           "boolean and null builtins retain status with non-help operands");
    expect(g_shell->execute("dispatch_value=\n"
                            "if_value=plain\n"
                            "for_value=plain\n"
                            "select_value=plain\n"
                            "while_value=plain\n"
                            "until_value=plain\n"
                            "case_value=plain\n"
                            "for n in if for select while until case; do\n"
                            "dispatch_value=\"$dispatch_value $n\"\n"
                            "done\n"
                            "if\ttrue; then :; fi\n"
                            "while false; do dispatch_value=wrong; done\n"
                            "until true; do dispatch_value=wrong; done\n") == 0 &&
               cjsh_env::get_shell_variable_value("dispatch_value") ==
                   " if for select while until case" &&
               cjsh_env::get_shell_variable_value("case_value") == "plain",
           "keyword dispatch preserves token boundaries, whitespace, and ordinary operands");
}

void test_escaped_whitespace(Parser& parser) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {R"(/tmp/Start\ VM.command one\ two)", {"/tmp/Start VM.command", "one two"}},
        {R"(: one\ \ two \ three)", {":", "one  two", " three"}},
        {": one\\\ttwo", {":", "one\ttwo"}},
        {R"(: one\ "two" one\ 'two')", {":", "one two", "one two"}},
        {R"(: one\\ two)", {":", "one\\", "two"}},
        {R"(export value=one\ two)", {"export", "value=one two"}},
    };
    for (const auto& [input, expected] : cases) {
        expect(parser.parse_command(input) == expected,
               "command parsing preserves escaped whitespace through expansion");
        const auto pipeline = parser.parse_pipeline(input + " | cat");
        expect(pipeline.size() == 2 && pipeline[0].args == expected &&
                   pipeline[1].args == std::vector<std::string>({"cat"}),
               "pipeline parsing preserves escaped whitespace through expansion");
    }
}

void test_redirection_argument_boundaries(Parser& parser) {
    for (const std::string argument : {"5 ", "5\t", "'5'", "\"5\"", "\\5", "5''"}) {
        const auto pipeline = parser.parse_pipeline("echo " + argument + ">output");
        expect(pipeline.size() == 1 &&
                   pipeline[0].args == std::vector<std::string>({"echo", "5"}) &&
                   pipeline[0].output_file == "output" && pipeline[0].fd_redirections.empty(),
               "spaced, quoted, and escaped numbers remain arguments before output redirection");
    }
    const auto attached = parser.parse_pipeline("echo 5>output");
    expect(attached.size() == 1 && attached[0].args == std::vector<std::string>({"echo"}) &&
               attached[0].output_file.empty() &&
               attached[0].fd_redirections ==
                   std::vector<std::pair<int, std::string>>({{5, "output:output"}}),
           "an adjacent unquoted number still selects the output descriptor");

    for (const std::string number : {"0", "2", "5", "10"}) {
        const auto input = parser.parse_pipeline("echo " + number + " <input");
        expect(input.size() == 1 && input[0].args == std::vector<std::string>({"echo", number}) &&
                   input[0].input_file == "input" && input[0].fd_redirections.empty(),
               "numeric arguments survive input redirection");
        const auto append = parser.parse_pipeline("echo " + number + " >>output");
        expect(append.size() == 1 && append[0].args == std::vector<std::string>({"echo", number}) &&
                   append[0].append_file == "output" && append[0].stderr_file.empty(),
               "numeric arguments survive append redirection");
        expect(Tokenizer::tokenize_command("echo " + number + " >&1") ==
                   std::vector<std::string>({"echo", number, ">&1"}),
               "a separated number does not change the duplicated descriptor");
    }
    expect(Tokenizer::tokenize_command("echo 1 2 >output 2>&1") ==
               std::vector<std::string>({"echo", "1", "2", ">", "output", "2>&1"}),
           "numeric arguments and attached descriptors can occur in the same command");
    expect(Tokenizer::tokenize_command("echo 10>&1 3<input 2>>error") ==
               std::vector<std::string>({"echo", "10>&1", "3<", "input", "2>>", "error"}),
           "attached duplication, input, and stderr append descriptors retain their meaning");
}

void test_redirection_path_expansion() {
    namespace fs = std::filesystem;
    const fs::path original_cwd = fs::current_path();
    const fs::path user_home = cjsh_filesystem::g_user_home_path();
    VariableExpander expander(g_shell.get(), cjsh_env::env_vars());
    Command plain;
    plain.output_file = "relative-output";
    plain.stderr_file = "/dev/null";
    expander.expand_command_paths_with_home(plain, "");
    expect(plain.output_file == "relative-output" && plain.stderr_file == "/dev/null",
           "ordinary redirection paths remain unchanged");

    for (const auto& directory : {original_cwd, original_cwd.parent_path()}) {
        fs::current_path(directory);
        Command command;
        command.input_file = "~/input";
        command.output_file = "~+/output";
        command.append_file = "~/append";
        command.stderr_file = "~-/error";
        command.both_output_file = "~/both";
        command.fd_redirections.emplace_back(3, "~+/extra");
        command.add_redirection(CommandRedirectionType::Output, "~+/ordered");
        expander.expand_command_paths_with_home(command, "");
        // The path helper expands ~/ and resolves other tilde forms relative
        // to cwd. Preserve that behavior while making directory reads lazy.
        expect(command.input_file == (user_home / "input").string() &&
                   command.output_file == (directory / "~+/output").string() &&
                   command.append_file == (user_home / "append").string() &&
                   command.stderr_file == (directory / "~-/error").string() &&
                   command.both_output_file == (user_home / "both").string() &&
                   command.fd_redirections[0].second == (directory / "~+/extra").string() &&
                   command.redirection_order[0].value == (directory / "~+/ordered").string(),
               "tilde redirections use the current directory on each invocation");
    }
    fs::current_path(original_cwd);
}
}  // namespace

int main() {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(false);
    config::interactive_mode = false;
    config::force_interactive = false;
    g_shell = std::make_unique<Shell>();
    g_shell->set_interactive_mode(false);
    test_logical_commands(*g_shell->get_parser());
    test_semicolon_commands(*g_shell->get_parser());
    test_comments();
    test_ampersand_commands();
    test_help();
    test_execution();
    test_escaped_whitespace(*g_shell->get_parser());
    test_redirection_argument_boundaries(*g_shell->get_parser());
    test_redirection_path_expansion();
    g_shell.reset();
    if (failures != 0) {
        (void)std::fprintf(stderr, "%zu/%zu parser dispatch tests failed\n", failures, checks);
        return 1;
    }
    std::printf("All %zu parser dispatch tests passed\n", checks);
    return 0;
}
