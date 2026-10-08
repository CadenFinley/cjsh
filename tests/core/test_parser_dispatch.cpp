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

#include <glob.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "expansion_engine.h"

#include "builtin_help.h"
#include "cjsh_filesystem.h"
#include "interpreter_utils.h"
#include "parser.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "tokenizer.h"
#include "variable_expander.h"

std::unique_ptr<Shell> shell;

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

    for (const std::string expression :
         {"$((1))", "$(((1 + 2) * 3))", "$((1 + $((2))))", "$((1 && (2 || 0)))", "\"$((1))\"",
          "$(printf '%s' \"$((1))\")"}) {
        const std::string first = "echo " + expression + " ";
        const auto arithmetic = parser.parse_logical_commands(first + "&& : yes || : no");
        expect(arithmetic.size() == 3 && arithmetic[0].command == first &&
                   arithmetic[0].op == "&&" && arithmetic[1].command == " : yes " &&
                   arithmetic[1].op == "||" && arithmetic[2].command == " : no",
               "arithmetic and nested substitutions preserve subsequent logical operators");
    }
    const auto grouped_arithmetic =
        parser.parse_logical_commands("(echo $((1)) && : inside) || : outside");
    expect(grouped_arithmetic.size() == 2 &&
               grouped_arithmetic[0].command == "(echo $((1)) && : inside) " &&
               grouped_arithmetic[0].op == "||",
           "arithmetic expansion does not close the surrounding subshell early");
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
    expect(shell->execute("dispatch_value=initial\n"
                          "false && dispatch_value=wrong\n"
                          "true || dispatch_value=wrong\n"
                          "true && dispatch_value='one;two'\n"
                          "dispatch_value=\"${dispatch_value#one;}\" # comment\n") == 0 &&
               cjsh_env::get_shell_variable_value("dispatch_value") == "two",
           "execution retains short circuiting, assignments, quotes, and parameter expansion");
    expect(shell->execute("dispatch_fn() { dispatch_value=$1; }\n"
                          "dispatch_fn first\ndispatch_fn second\n") == 0 &&
               cjsh_env::get_shell_variable_value("dispatch_value") == "second",
           "repeated function calls see current positional parameters");
    expect(shell->execute(": ignored --help") == 0 && shell->execute("true ignored --help") == 0 &&
               shell->execute("false ignored --help") == 1,
           "boolean and null builtins retain status with non-help operands");
    expect(shell->execute("dispatch_value=\n"
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

void test_repeated_expansion(Parser& parser) {
    for (const char* value : {"first", "second", ""}) {
        shell->execute(std::string("profile_value='") + value + "'");
        expect(
            parser.parse_command(": \"$profile_value\"") == std::vector<std::string>({":", value}),
            "repeated command text expands the current variable value");
        const auto pipeline = parser.parse_pipeline_with_preprocessing(
            ": \"$profile_value\" >\"out-$profile_value\"");
        expect(pipeline.size() == 1 && pipeline[0].args == std::vector<std::string>({":", value}) &&
                   pipeline[0].output_file == std::string("out-") + value &&
                   pipeline[0].redirection_order.size() == 1 &&
                   pipeline[0].redirection_order[0].value == std::string("out-") + value,
               "repeated pipeline tokens expand current arguments and ordered redirection targets");
    }
    for (const char* value : {"first", "second"}) {
        parser.set_aliases({{"profile_alias", std::string(": ") + value}});
        expect(parser.parse_command("profile_alias 'arg'") ==
                   std::vector<std::string>({":", value, "arg"}),
               "repeated command text uses current aliases");
        const auto pipeline =
            parser.parse_pipeline_with_preprocessing("profile_alias 'arg' >/dev/null");
        expect(pipeline.size() == 1 &&
                   pipeline[0].args == std::vector<std::string>({":", value, "arg"}),
               "repeated pipeline tokens use current aliases");
    }
    parser.set_aliases({});
    shell->execute("profile_value='a:b c'");
    shell->execute("IFS=:");
    expect(parser.parse_command("echo $profile_value") ==
               std::vector<std::string>({"echo", "a", "b c"}),
           "field splitting uses changed IFS");
    expect(parser.parse_pipeline_with_preprocessing("echo $profile_value >/dev/null")[0].args ==
               std::vector<std::string>({"echo", "a", "b c"}),
           "pipeline field splitting uses current IFS");
    shell->execute("IFS=' '");
    expect(parser.parse_command("echo $profile_value") ==
               std::vector<std::string>({"echo", "a:b", "c"}),
           "repeated command text does not cache IFS");
    expect(parser.parse_pipeline_with_preprocessing("echo $profile_value >/dev/null")[0].args ==
               std::vector<std::string>({"echo", "a:b", "c"}),
           "repeated pipeline tokens do not cache IFS");
    shell->execute("unset IFS");
    const bool old_extglob = config::extglob_enabled;
    for (bool enabled : {false, true, false}) {
        config::extglob_enabled = enabled;
        Parser fresh;
        fresh.set_shell(shell.get());
        expect(parser.parse_command(": @(missing-one|missing-two)") ==
                   fresh.parse_command(": @(missing-one|missing-two)"),
               "tokenization reflects extglob changes");
        expect(parser.tokenize_command_cached(": @(missing-one|missing-two) >/dev/null") ==
                   Tokenizer::tokenize_command(": @(missing-one|missing-two) >/dev/null"),
               "shared lexical tokens reflect extglob changes");
    }
    config::extglob_enabled = old_extglob;

    const auto saved_dialect = config::shell_dialect();
    for (const auto dialect :
         {config::ShellDialect::Cjsh, config::ShellDialect::Posix, config::ShellDialect::Cjsh}) {
        config::set_shell_dialect(dialect);
        expect(parser.tokenize_command_cached(": a'quoted'b >out") ==
                   Tokenizer::tokenize_command(": a'quoted'b >out"),
               "shared lexical tokens reflect dialect changes");
    }
    config::set_shell_dialect(config::ShellDialect::Posix);
    // Tilde tokenization also protects characters selected by the current IFS.
    const std::string saved_ifs = cjsh_env::get_shell_variable_value("IFS");
    const bool had_ifs = cjsh_env::shell_variable_is_set("IFS");
    for (const char* ifs : {"/", ":", " "}) {
        (void)cjsh_env::set_shell_variable_value("IFS", ifs);
        expect(parser.tokenize_command_cached(": ~ >/dev/null") ==
                   Tokenizer::tokenize_command(": ~ >/dev/null"),
               "POSIX tilde tokens are refreshed after environment changes");
    }
    if (had_ifs) {
        (void)cjsh_env::set_shell_variable_value("IFS", saved_ifs);
    } else {
        (void)cjsh_env::unset_shell_variable_value("IFS");
    }
    config::set_shell_dialect(saved_dialect);
}

void test_simple_glob_matches_libc() {
    namespace fs = std::filesystem;
    const auto original_cwd = fs::current_path();
    std::string directory_template = (fs::temp_directory_path() / "cjsh-glob-XXXXXX").string();
    const char* created = mkdtemp(directory_template.data());
    expect(created != nullptr, "create an isolated glob fixture");
    if (created == nullptr) {
        return;
    }
    const fs::path root(created);
    fs::create_directories(root / "folder");
    for (const char* name : {"z.txt", "a.txt", "b.c", ".hidden", "a b", "a*", "a?", "A.txt"}) {
        std::ofstream(root / name) << "fixture";
    }
    fs::create_symlink("folder", root / "dirlink");
    fs::create_symlink("a.txt", root / "filelink");
    fs::create_symlink("missing", root / "broken");
    std::ofstream(root / "folder" / "nested.txt") << "nested";
    fs::current_path(root);
    ExpansionEngine expansion;
    for (bool unicode_file : {false, true}) {
        if (unicode_file) {
            std::ofstream(root / "é.txt") << "fixture";
        }
        for (const std::string& prefix :
             std::vector<std::string>{"", "./", root.string() + "/", root.string() + "//"}) {
            for (const char* suffix :
                 {"*",      "*.txt",  "a?",           ".*",           "f*",
                  "*link",  "b*",     "no*",          "[ab]*",        "[!a]*",
                  "[^a]*",  "[a-z]*", "[[:alpha:]]*", "[[:upper:]]*", "[.]hidden",
                  "[",      "[[]*",   "[]a]*",        "[z-a]*",       "[bf]*/",
                  "[fd]*/", "*/",     "folder/*",     "[fd]*/*",      "f[io]*link"}) {
                const auto pattern = prefix + suffix;
                glob_t results{};
                const int status = glob(pattern.c_str(), GLOB_TILDE | GLOB_MARK, nullptr, &results);
                std::vector<std::string> expected;
                if (status == 0) {
                    expected.assign(results.gl_pathv, results.gl_pathv + results.gl_pathc);
                } else if (status == GLOB_NOMATCH) {
                    expected.push_back(pattern);
                }
                globfree(&results);
                expect(expansion.expand_wildcards(pattern) == expected,
                       ("glob matches libc: " + pattern).c_str());
            }
        }
    }
    const bool old_extglob = config::extglob_enabled;
    config::extglob_enabled = true;
    expect(expansion.expand_wildcards("@(filelink|dirlink|broken)") ==
               std::vector<std::string>{"broken", "dirlink/", "filelink"},
           "extglob preserves file, directory, and dangling symlink matches");
    expect(expansion.expand_wildcards("@(filelink|dirlink|broken)/") ==
               std::vector<std::string>{"dirlink/"},
           "extglob trailing slash keeps only directories");
    expect(expansion.expand_wildcards("@(folder|dirlink)/*.txt") ==
               std::vector<std::string>{"dirlink/nested.txt", "folder/nested.txt"},
           "extglob preserves traversal through directory symlinks");
    config::extglob_enabled = old_extglob;
    fs::current_path(original_cwd);
    fs::remove_all(root);
}

void test_redirection_path_expansion() {
    namespace fs = std::filesystem;
    const fs::path original_cwd = fs::current_path();
    const fs::path& user_home = cjsh_filesystem::g_user_home_path();
    VariableExpander expander(shell.get(), cjsh_env::env_vars());
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
    shell = std::make_unique<Shell>();
    shell->set_interactive_mode(false);
    test_logical_commands(*shell->get_parser());
    test_semicolon_commands(*shell->get_parser());
    test_comments();
    test_ampersand_commands();
    test_help();
    test_execution();
    test_escaped_whitespace(*shell->get_parser());
    test_redirection_argument_boundaries(*shell->get_parser());
    test_redirection_path_expansion();
    test_repeated_expansion(*shell->get_parser());
    test_simple_glob_matches_libc();
    shell.reset();
    if (failures != 0) {
        (void)std::fprintf(stderr, "%zu/%zu parser dispatch tests failed\n", failures, checks);
        return 1;
    }
    std::printf("All %zu parser dispatch tests passed\n", checks);
    return 0;
}
