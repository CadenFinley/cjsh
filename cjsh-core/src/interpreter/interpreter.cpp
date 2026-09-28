/*
  interpreter.cpp

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

#include "interpreter.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "arithmetic_evaluator.h"
#include "builtin.h"
#include "case_evaluator.h"
#include "command_substitution_evaluator.h"
#include "conditional_evaluator.h"
#include "coproc_command.h"
#include "error_out.h"
#include "exec.h"
#include "flags.h"
#include "function_evaluator.h"
#include "function_ref.h"
#include "interpreter_utils.h"
#include "job_control.h"
#include "loop_evaluator.h"
#include "numeric_utils.h"
#include "parameter_expansion_evaluator.h"
#include "parser.h"
#include "parser_utils.h"
#include "pipeline_status_utils.h"
#include "quote_info.h"
#include "readonly_command.h"
#include "redirection_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"
#include "string_utils.h"
#include "suggestion_utils.h"
#include "tokenizer.h"
#include "variable_manager.h"
#include "wait_status_utils.h"

using shell_script_interpreter::detail::contains_token;
using shell_script_interpreter::detail::is_control_flow_exit_code;
using shell_script_interpreter::detail::is_readable_file;
using shell_script_interpreter::detail::process_line_for_validation;
using shell_script_interpreter::detail::should_skip_line;
using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;

namespace {

thread_local bool g_parameter_expansion_fatal_error = false;

constexpr std::string_view kSignalExitExceptionPrefix = "__CJSH_SIGNAL_EXIT__:";

std::optional<int> collect_pending_signal_exit_code() {
    if (!g_shell) {
        return std::nullopt;
    }

    SignalProcessingResult pending = g_shell->process_pending_signals();
    int exit_code = shell_script_interpreter::detail::pending_signal_exit_code(pending);
    if (cjsh_env::exit_requested()) {
        return numeric_utils::parse_exit_status_or(cjsh_env::get_shell_variable_value("EXIT_CODE"),
                                                   exit_code < 0 ? 0 : exit_code, false);
    }
    if (exit_code < 0) {
        return std::nullopt;
    }
    return exit_code;
}

bool is_terminating_signal_exit_code(int exit_code) {
#ifdef SIGINT
    return exit_code == 128 + SIGINT;
#else
    return false;
#endif
}

std::runtime_error make_signal_exit_exception(int exit_code) {
    return std::runtime_error(std::string(kSignalExitExceptionPrefix) + std::to_string(exit_code));
}

std::optional<int> parse_signal_exit_exception(std::string_view message) {
    if (message.rfind(kSignalExitExceptionPrefix, 0) != 0) {
        return std::nullopt;
    }

    std::string code_text(message.substr(kSignalExitExceptionPrefix.size()));
    if (code_text.empty()) {
        return std::nullopt;
    }

    try {
        return std::stoi(code_text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

enum class LogicalOperator : std::uint8_t {
    And,
    Or
};

std::optional<LogicalOperator> parse_logical_operator(std::string_view op) {
    if (op == "&&") {
        return LogicalOperator::And;
    }
    if (op == "||") {
        return LogicalOperator::Or;
    }
    return std::nullopt;
}

struct ParsedAssignmentToken {
    std::string lhs;
    std::string rhs;
    bool append = false;
};

bool is_identifier_or_indexed_target(const std::string& lhs) {
    if (is_valid_identifier(lhs)) {
        return true;
    }

    size_t left_bracket = lhs.find('[');
    if (left_bracket == std::string::npos || lhs.back() != ']') {
        return false;
    }

    std::string name = lhs.substr(0, left_bracket);
    if (!is_valid_identifier(name)) {
        return false;
    }

    return (left_bracket + 2) < lhs.size();
}

bool parse_assignment_token(const std::string& token, ParsedAssignmentToken& parsed) {
    std::string lhs;
    std::string rhs;
    if (!split_on_first_equals(token, lhs, rhs, true)) {
        return false;
    }

    lhs = trim_whitespace(lhs);
    if (lhs.empty()) {
        return false;
    }

    bool append = false;
    if (!lhs.empty() && lhs.back() == '+') {
        append = true;
        lhs.pop_back();
    }

    if (!is_identifier_or_indexed_target(lhs)) {
        return false;
    }

    parsed.lhs = std::move(lhs);
    parsed.rhs = std::move(rhs);
    parsed.append = append;
    return true;
}

std::string assignment_base_name(const std::string& lhs) {
    size_t left_bracket = lhs.find('[');
    if (left_bracket == std::string::npos) {
        return lhs;
    }
    return lhs.substr(0, left_bracket);
}

bool parse_array_literal_assignment(const std::vector<std::string>& args, std::string& name,
                                    std::vector<std::string>& words, bool& append) {
    if (args.size() < 3) {
        return false;
    }

    ParsedAssignmentToken parsed;
    if (!parse_assignment_token(args[0], parsed) || !parsed.rhs.empty()) {
        return false;
    }

    if (parsed.lhs.find('[') != std::string::npos) {
        return false;
    }

    if (args[1] != "(") {
        return false;
    }

    int depth = 0;
    size_t closing_index = std::string::npos;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "(") {
            ++depth;
        } else if (args[i] == ")") {
            --depth;
            if (depth == 0) {
                closing_index = i;
                break;
            }
            if (depth < 0) {
                return false;
            }
        }
    }

    if (depth != 0 || closing_index == std::string::npos || closing_index != args.size() - 1) {
        return false;
    }

    name = parsed.lhs;
    append = parsed.append;
    words.assign(args.begin() + 2, args.end() - 1);
    return true;
}

enum class StatementKeyword : std::uint8_t {
    If,
    For,
    Select,
    While,
    Until,
    Case
};

std::optional<StatementKeyword> parse_statement_keyword_prefix(std::string_view text) {
    if (parser_starts_with_keyword_token(text, "if")) {
        return StatementKeyword::If;
    }
    if (parser_starts_with_keyword_token(text, "for")) {
        return StatementKeyword::For;
    }
    if (parser_starts_with_keyword_token(text, "select")) {
        return StatementKeyword::Select;
    }
    if (parser_starts_with_keyword_token(text, "while")) {
        return StatementKeyword::While;
    }
    if (parser_starts_with_keyword_token(text, "until")) {
        return StatementKeyword::Until;
    }
    if (parser_starts_with_keyword_token(text, "case")) {
        return StatementKeyword::Case;
    }
    return std::nullopt;
}

bool is_statement_keyword_prefix(std::string_view text, StatementKeyword keyword) {
    // Callers already know the keyword they need; do not classify the same
    // command against every other statement keyword first.
    switch (keyword) {
        case StatementKeyword::If:
            return parser_starts_with_keyword_token(text, "if");
        case StatementKeyword::For:
            return parser_starts_with_keyword_token(text, "for");
        case StatementKeyword::Select:
            return parser_starts_with_keyword_token(text, "select");
        case StatementKeyword::While:
            return parser_starts_with_keyword_token(text, "while");
        case StatementKeyword::Until:
            return parser_starts_with_keyword_token(text, "until");
        case StatementKeyword::Case:
            return parser_starts_with_keyword_token(text, "case");
    }
    return false;
}

bool is_loop_keyword(StatementKeyword keyword) {
    switch (keyword) {
        case StatementKeyword::For:
        case StatementKeyword::Select:
        case StatementKeyword::While:
        case StatementKeyword::Until:
            return true;
        case StatementKeyword::If:
        case StatementKeyword::Case:
            return false;
    }
    return false;
}

using RedirectOperator = redirection_utils::RedirectionOperator;

std::optional<RedirectOperator> parse_redirect_operator(std::string_view token) {
    return redirection_utils::parse_operator_token(token);
}

bool redirect_requires_operand(RedirectOperator op) {
    return redirection_utils::requires_operand(op);
}

int report_error_with_code(ErrorType type, ErrorSeverity severity, const std::string& command,
                           const std::string& message, const std::vector<std::string>& suggestions,
                           int code) {
    print_error({type, severity, command, message, suggestions});
    pipeline_status_utils::set_last_status_env(code);
    return code;
}

std::string sanitize_context(const std::string& text) {
    std::string sanitized = text;
    (void)sanitized.erase(std::remove(sanitized.begin(), sanitized.end(), '\n'), sanitized.end());
    (void)sanitized.erase(std::remove(sanitized.begin(), sanitized.end(), '\r'), sanitized.end());
    sanitized = trim(sanitized);
    if (sanitized.size() > 160) {
        sanitized.resize(157);
        (void)sanitized.append("...");
    }
    return sanitized;
}

void append_context_hint(std::vector<std::string>& suggestions, const std::string& text,
                         size_t line_number) {
    if (!config::error_suggestions_enabled) {
        return;
    }
    if (text.empty() && line_number == 0) {
        return;
    }

    std::ostringstream builder;
    std::string sanitized = sanitize_context(text);
    if (line_number > 0) {
        builder << "line " << line_number;
        if (!sanitized.empty()) {
            builder << ": " << sanitized;
        }
    } else if (!sanitized.empty()) {
        builder << sanitized;
    }

    if (builder.tellp() > 0) {
        suggestions.push_back("Context: " + builder.str());
    }
}

std::string strip_cjsh_prefix(std::string message) {
    const std::string prefix = "cjsh:";
    if (message.rfind(prefix, 0) == 0) {
        (void)message.erase(0, prefix.size());
        message = string_utils::trim_left_ascii_whitespace_copy(message);
    }
    if (message.rfind("cjsh ", 0) == 0) {
        (void)message.erase(0, 5);
        message = string_utils::trim_left_ascii_whitespace_copy(message);
    }
    return message;
}

std::vector<std::string> build_command_suggestions(const std::string& command_name) {
    return suggestion_utils::generate_command_suggestions_if_enabled(command_name);
}

int handle_runtime_exception(const std::string& text, const std::runtime_error& e,
                             size_t line_number) {
    const std::string raw_message = e.what() ? std::string(e.what()) : "runtime error";

    if (auto signal_exit = parse_signal_exit_exception(raw_message)) {
        return *signal_exit;
    }

    std::string message = strip_cjsh_prefix(raw_message);
    std::vector<std::string> suggestions;
    auto add_context = [&] { append_context_hint(suggestions, text, line_number); };

    const std::string needle = "command not found: ";
    size_t pos = raw_message.find(needle);
    if (pos != std::string::npos) {
        std::string command_name = trim(raw_message.substr(pos + needle.length()));
        if (command_name.empty()) {
            command_name = trim(text);
        }
        suggestions = build_command_suggestions(command_name);
        add_context();
        return report_error_with_code(ErrorType::COMMAND_NOT_FOUND, ErrorSeverity::ERROR,
                                      command_name, "", suggestions,
                                      ShellScriptInterpreter::exit_command_not_found);
    }

    if (message == "expected a command after '!'") {
        if (config::error_suggestions_enabled) {
            suggestions.push_back("'!' inverts a command's exit status. Try '! false'.");
        }
        add_context();
        return report_error_with_code(ErrorType::SYNTAX_ERROR, ErrorSeverity::ERROR, "", message,
                                      suggestions, 2);
    }

    if (message.find("Unclosed quote") != std::string::npos ||
        message.find("missing closing") != std::string::npos ||
        message.find("syntax error near unexpected token") != std::string::npos) {
        if (config::error_suggestions_enabled) {
            suggestions.push_back("Make sure all quotes and delimiters are balanced.");
        }
        add_context();
        return report_error_with_code(ErrorType::SYNTAX_ERROR, ErrorSeverity::ERROR, "", message,
                                      suggestions, 2);
    }

    if (message.find("Failed to open") != std::string::npos ||
        message.find("Failed to redirect") != std::string::npos ||
        message.find("Failed to write") != std::string::npos) {
        if (config::error_suggestions_enabled) {
            suggestions.push_back("Check file permissions and paths.");
        }
        add_context();
        return report_error_with_code(ErrorType::FILE_NOT_FOUND, ErrorSeverity::ERROR, "", message,
                                      suggestions, 2);
    }

    if (message.find("parameter expansion error:") != std::string::npos) {
        g_parameter_expansion_fatal_error = true;
    }

    if (config::error_suggestions_enabled) {
        suggestions.push_back("Check command syntax and system resources.");
    }
    add_context();
    return report_error_with_code(ErrorType::RUNTIME_ERROR, ErrorSeverity::ERROR, "", message,
                                  suggestions, 2);
}

}  // namespace

ShellScriptInterpreter::ShellScriptInterpreter() : shell_parser(nullptr) {
}

ShellScriptInterpreter::~ShellScriptInterpreter() = default;

void ShellScriptInterpreter::set_parser(Parser* parser) {
    shell_parser = parser;
}

void ShellScriptInterpreter::set_error_source(const std::string& source) {
    error_source = source;
}

const std::string& ShellScriptInterpreter::get_error_source() const {
    return error_source;
}

std::vector<std::string> ShellScriptInterpreter::parse_into_lines(const std::string& script) {
    if (!shell_parser) {
        return {};
    }
    return shell_parser->parse_into_lines(script);
}

ShellScriptInterpreter::SyntaxError::SyntaxError(size_t line_num, const std::string& msg,
                                                 const std::string& line_content)
    : position({line_num, 0, 0, 0}),
      severity(ErrorSeverity::ERROR),
      category(ErrorCategory::SYNTAX),
      error_code("SYN001"),
      message(msg),
      line_content(line_content) {
}

ShellScriptInterpreter::SyntaxError::SyntaxError(ErrorPosition pos, ErrorSeverity sev,
                                                 ErrorCategory cat, const std::string& code,
                                                 const std::string& msg,
                                                 const std::string& line_content,
                                                 const std::string& suggestion)
    : position(pos),
      severity(sev),
      category(cat),
      error_code(code),
      message(msg),
      line_content(line_content),
      suggestion(suggestion) {
}

VariableManager& ShellScriptInterpreter::get_variable_manager() {
    return variable_manager;
}

int ShellScriptInterpreter::execute_subshell(const std::string& subshell_content) {
    pid_t pid = fork();
    if (pid == 0) {
        if (setpgid(0, 0) < 0) {
            print_error_errno(
                {ErrorType::RUNTIME_ERROR, "subshell", "setpgid failed in subshell child", {}});
        }

        int exit_code = g_shell->execute(subshell_content, true);
        exit_code = read_exit_code_or(exit_code);

        int child_status = 0;
        while (waitpid(-1, &child_status, WNOHANG) > 0) {
        }

        // exit() destroys function-local statics (including JobManager) before
        // the inherited shell cleanup callback, which then accesses freed state.
        // Run shell hooks explicitly and leave C++ teardown to the parent.
        g_shell->run_exit_handlers(exit_code);
        (void)std::cout.flush();
        (void)std::cerr.flush();
        (void)std::clog.flush();
        (void)std::fflush(nullptr);
        _exit(exit_code);
    } else if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        int exit_code = wait_status_utils::to_exit_code(status, 1);
        return set_last_status(exit_code);
    } else {
        print_error({ErrorType::RUNTIME_ERROR,
                     ErrorSeverity::ERROR,
                     "subshell",
                     "failed to fork for subshell execution",
                     {"Check system process limits."}});
        return 1;
    }
}

int ShellScriptInterpreter::execute_function_call(const std::vector<std::string>& expanded_args) {
    if (expanded_args.empty()) {
        return set_last_status(0);
    }

    auto function_it = functions.find(expanded_args[0]);
    if (function_it == functions.end()) {
        return set_last_status(127);
    }

    const function_evaluator::FunctionDefinition& function_definition = function_it->second;

    push_function_scope();

    std::vector<std::string> saved_params = flags::get_positional_parameters();

    std::vector<std::string> func_params;
    for (size_t pi = 1; pi < expanded_args.size(); ++pi) {
        func_params.push_back(expanded_args[pi]);
    }
    flags::set_positional_parameters(func_params);

    std::vector<std::string> param_names;
    for (size_t pi = 1; pi < expanded_args.size() && pi <= 9; ++pi) {
        std::string name = std::to_string(pi);
        param_names.push_back(name);
        (void)setenv(name.c_str(), expanded_args[pi].c_str(), 1);
    }

    int exit_code = 0;
    if (function_definition.uses_subshell_body) {
        std::ostringstream body_stream;
        for (size_t body_line_index = 0; body_line_index < function_definition.body_lines.size();
             ++body_line_index) {
            if (body_line_index > 0) {
                body_stream << '\n';
            }
            body_stream << function_definition.body_lines[body_line_index];
        }
        exit_code = execute_subshell(body_stream.str());
    } else {
        exit_code = execute_block(function_definition.body_lines);
    }

    if ((exit_code == exit_return) && cjsh_env::shell_variable_is_set("CJSH_RETURN_CODE")) {
        std::string return_code_env = cjsh_env::get_shell_variable_value("CJSH_RETURN_CODE");
        try {
            exit_code = std::stoi(return_code_env);
            (void)cjsh_env::unset_shell_variable_value("CJSH_RETURN_CODE");
        } catch (const std::exception&) {
            exit_code = 0;
        }
    }

    flags::set_positional_parameters(saved_params);

    for (const auto& n : param_names) {
        (void)unsetenv(n.c_str());
    }

    pop_function_scope();

    return set_last_status(exit_code);
}

int ShellScriptInterpreter::invoke_function(const std::vector<std::string>& args) {
    if (args.empty()) {
        return set_last_status(0);
    }

    auto it = functions.find(args[0]);
    if (it == functions.end()) {
        return set_last_status(127);
    }

    return execute_function_call(args);
}

int ShellScriptInterpreter::handle_env_assignment(const std::vector<std::string>& expanded_args) {
    if (expanded_args.empty()) {
        return -1;
    }

    auto assignment_success_status = [this] {
        int status =
            pending_assignment_exit_status.value_or(last_substitution_exit_status.value_or(0));
        last_substitution_exit_status.reset();
        pending_assignment_exit_status.reset();
        return status;
    };

    auto clear_assignment_status = [this] {
        last_substitution_exit_status.reset();
        pending_assignment_exit_status.reset();
    };

    std::string literal_name;
    std::vector<std::string> literal_words;
    bool literal_append = false;
    if (parse_array_literal_assignment(expanded_args, literal_name, literal_words,
                                       literal_append)) {
        if (!readonly_manager_can_assign(literal_name, "assignment")) {
            clear_assignment_status();
            return 1;
        }

        if (!variable_manager.assign_array_literal(literal_name, literal_words, literal_append)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "assignment",
                         "invalid array assignment for '" + literal_name + "'",
                         {}});
            clear_assignment_status();
            return 1;
        }

        return assignment_success_status();
    }

    if (expanded_args.size() != 1) {
        return -1;
    }

    ParsedAssignmentToken parsed;
    if (!parse_assignment_token(expanded_args[0], parsed)) {
        return -1;
    }

    std::string base_name = assignment_base_name(parsed.lhs);
    if (!readonly_manager_can_assign(base_name, "assignment")) {
        clear_assignment_status();
        return 1;
    }

    if (!variable_manager.assign_variable(parsed.lhs, parsed.rhs, parsed.append)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "assignment",
                     "invalid assignment target: " + parsed.lhs,
                     {}});
        clear_assignment_status();
        return 1;
    }

    return assignment_success_status();
}

int ShellScriptInterpreter::execute_block(const std::vector<std::string>& lines,
                                          bool skip_validation) {
    struct ValidationScope {
        ShellScriptInterpreter* self;
        bool previous;
        ValidationScope(ShellScriptInterpreter* s, bool skip)
            : self(s), previous(s->skip_validation_mode) {
            if (skip) {
                self->skip_validation_mode = true;
            }
        }

        ~ValidationScope() {
            self->skip_validation_mode = previous;
        }
    } validation_scope(this, skip_validation);

    if (g_shell) {
        g_shell->mark_terminal_dirty();
    }
    const bool effective_skip = skip_validation_mode;
    if (!effective_skip) {
        g_parameter_expansion_fatal_error = false;
    }

    if (g_shell == nullptr) {
        print_error({ErrorType::FATAL_ERROR, "", "shell not initialized properly", {}});
    }

    if (shell_parser == nullptr) {
        std::vector<std::string> empty_suggestions;
        ErrorInfo error(ErrorType::FATAL_ERROR, ErrorSeverity::CRITICAL, "",
                        "shell not initialized properly", empty_suggestions);
        print_error(error);
        return 1;
    }

    if (!effective_skip && has_syntax_errors(lines)) {
        std::vector<std::string> empty_suggestions;
        ErrorInfo error(ErrorType::SYNTAX_ERROR, ErrorSeverity::CRITICAL, "",
                        "Critical syntax errors detected in script block, process aborted",
                        empty_suggestions);
        print_error(error);
        return 2;
    }

    std::function<int(const std::string&, bool, bool*)> execute_simple_or_pipeline_impl;
    std::function<int(const std::string&)> execute_simple_or_pipeline;
    bool last_result_errexit_exempt = false;

    std::function<int(const std::string&)> evaluate_logical_condition;
    std::function<std::optional<int>(const std::string&, bool)> try_handle_inline_case;

    execute_simple_or_pipeline = [&](const std::string& cmd_text) -> int {
        return execute_simple_or_pipeline_impl(cmd_text, true, nullptr);
    };

    evaluate_logical_condition = [&](const std::string& condition) -> int {
        // conditions from if and elif headers are normalized through this shared path
        return evaluate_logical_condition_internal(condition, execute_simple_or_pipeline);
    };

    execute_simple_or_pipeline_impl = [&](const std::string& cmd_text, bool allow_semicolon_split,
                                          bool* function_call) -> int {
        if (function_call) {
            *function_call = false;
        }
        auto call_function = [&](const std::vector<std::string>& args) {
            if (function_call) {
                *function_call = true;
            }
            return execute_function_call(args);
        };
        last_result_errexit_exempt = false;
        if (SignalHandler::startup_interrupted() && !SignalHandler::executing_trap()) {
            return set_last_status(128 + SIGINT);
        }
        // single-command executor used by if conditions and by branch body commands
        std::string text = process_line_for_validation(cmd_text);
        if (text.empty()) {
            return 0;
        }

        auto has_control_operators = [](const std::string& input) {
            return input.find_first_of("|&;<>!(){}`") != std::string::npos;
        };

        auto try_execute_quick_command =
            [&](const std::string& command_text) -> std::optional<int> {
            if (!shell_parser || has_control_operators(command_text)) {
                return std::nullopt;
            }

            std::vector<std::string> quick_args = shell_parser->parse_command(command_text);
            if (quick_args.empty()) {
                return 0;
            }

            const std::string& program = quick_args[0];
            if (program == "coproc") {
                return std::nullopt;
            }
            if (should_interpret_as_cjsh_script(program)) {
                std::ifstream f(program);
                if (!f) {
                    print_error({ErrorType::RUNTIME_ERROR,
                                 "",
                                 "Failed to open script file: " + program,
                                 {}});
                    return 1;
                }
                std::stringstream buffer;
                buffer << f.rdbuf();
                auto nested_lines = shell_parser->parse_into_lines(buffer.str());
                return execute_block(nested_lines);
            }

            int env_result = handle_env_assignment(quick_args);
            if (env_result >= 0) {
                return env_result;
            }

            if (functions.count(program) != 0U) {
                return call_function(quick_args);
            }

            auto prepared = cjsh_env::prepare_command(std::move(quick_args));
            if (!prepared.assignments.empty() && !prepared.args.empty() &&
                functions.count(prepared.args.front()) != 0U) {
                Command function_command;
                function_command.args = std::move(prepared.original_args);
                if (function_call) {
                    *function_call = true;
                }
                return run_pipeline({function_command});
            }

            int exit_code = g_shell->execute_prepared_command(std::move(prepared));
            return set_last_status(exit_code);
        };

        if (auto quick_result = try_execute_quick_command(text)) {
            return *quick_result;
        }

        auto command_has_redirection = [](const Command& command) {
            return command.stderr_to_stdout || command.stdout_to_stderr ||
                   !command.input_file.empty() || !command.output_file.empty() ||
                   !command.append_file.empty() || !command.stderr_file.empty() ||
                   !command.here_doc.empty();
        };

        if (shell_parser &&
            (text.find("&&") != std::string::npos || text.find("||") != std::string::npos)) {
            std::vector<LogicalCommand> logical_cmds = shell_parser->parse_logical_commands(text);
            bool has_logical_op = false;
            for (const auto& lc : logical_cmds) {
                if (!lc.op.empty()) {
                    has_logical_op = true;
                    break;
                }
            }

            if (has_logical_op) {
                int logical_status = 0;
                size_t last_executed_index = 0;
                bool executed_command = false;
                for (size_t idx = 0; idx < logical_cmds.size(); ++idx) {
                    if (idx > 0) {
                        const std::string& prev_op = logical_cmds[idx - 1].op;
                        bool is_control_flow = is_control_flow_exit_code(logical_status);
                        auto op = parse_logical_operator(prev_op);
                        if (op.has_value()) {
                            if (*op == LogicalOperator::And && logical_status != 0 &&
                                !is_control_flow) {
                                continue;
                            }
                            if (*op == LogicalOperator::Or && logical_status == 0) {
                                continue;
                            }
                        }
                        if (is_control_flow) {
                            break;
                        }
                    }

                    last_executed_index = idx;
                    executed_command = true;
                    logical_status =
                        execute_simple_or_pipeline_impl(logical_cmds[idx].command, true, nullptr);

                    if (is_terminating_signal_exit_code(logical_status)) {
                        return logical_status;
                    }
                }
                last_result_errexit_exempt = logical_status != 0 && executed_command &&
                                             last_executed_index + 1 < logical_cmds.size();
                return logical_status;
            }
        }

        if (allow_semicolon_split && shell_parser && text.find(';') != std::string::npos) {
            auto semicolon_commands = shell_parser->parse_semicolon_commands(text);

            if (semicolon_commands.size() > 1) {
                int last_code = 0;
                for (const auto& part : semicolon_commands) {
                    last_code = execute_simple_or_pipeline_impl(part, false, nullptr);
                    const bool errexit_exempt = last_result_errexit_exempt;

                    if (is_terminating_signal_exit_code(last_code)) {
                        return last_code;
                    }

                    if (g_parameter_expansion_fatal_error) {
                        return last_code;
                    }

                    if (g_shell && g_shell->should_abort_on_nonzero_exit(last_code) &&
                        last_code != 0 && !is_control_flow_exit_code(last_code) &&
                        !errexit_exempt) {
                        return last_code;
                    }
                }
                return last_code;
            }
        }

        if (text == "coproc" || (text.size() > 6 && text.rfind("coproc", 0) == 0 &&
                                 std::isspace(static_cast<unsigned char>(text[6])) != 0)) {
            return set_last_status(coproc_script_command(text, g_shell.get()));
        }

        std::vector<std::string> parsed_args;
        std::vector<Command> cmds;
        bool has_redir_or_pipe = false;
        bool has_multiple_commands = false;
        std::string trimmed_text = trim(text);

        bool negate_arithmetic_status = false;
        std::string arithmetic_command_expression;
        if (parser_parse_arithmetic_command_form(trimmed_text, negate_arithmetic_status,
                                                 arithmetic_command_expression)) {
            try {
                std::string expanded_expression = expand_all_substitutions(
                    arithmetic_command_expression, execute_simple_or_pipeline);
                if (shell_parser != nullptr) {
                    shell_parser->expand_env_vars(expanded_expression);
                }

                long long result = evaluate_arithmetic_expression(expanded_expression);
                int arithmetic_status = (result != 0) ? 0 : 1;
                if (negate_arithmetic_status) {
                    arithmetic_status = (arithmetic_status == 0) ? 1 : 0;
                }
                return set_last_status(arithmetic_status);
            } catch (const std::runtime_error& e) {
                return handle_runtime_exception(text, e, current_line_number);
            }
        }

        try {
            const size_t group_start = text.find_first_not_of(" \t\r\n");
            size_t group_end = std::string::npos;
            if (group_start != std::string::npos) {
                if (text[group_start] == '(') {
                    group_end = find_matching_paren(text, group_start);
                } else if (text[group_start] == '{' &&
                           parser_is_command_group_brace(text, group_start)) {
                    group_end = find_matching_brace(text, group_start);
                }
            }
            if (group_end != std::string::npos) {
                // The group evaluates its body in its own execution context.
                text = text.substr(0, group_end + 1) +
                       expand_all_substitutions(text.substr(group_end + 1),
                                                execute_simple_or_pipeline);
            } else {
                text = expand_all_substitutions(text, execute_simple_or_pipeline);
            }

            if (auto quick_result = try_execute_quick_command(text)) {
                return *quick_result;
            }

            auto merged_tokens = Tokenizer::tokenize_command(text);
            if (!merged_tokens.empty()) {
                auto requires_operand = [&](const std::string& token) -> bool {
                    if (auto redirect = parse_redirect_operator(token)) {
                        return redirect_requires_operand(*redirect);
                    }
                    size_t digits = 0;
                    while (digits < token.size() &&
                           (std::isdigit(static_cast<unsigned char>(token[digits])) != 0)) {
                        digits++;
                    }
                    if (digits == 0 || digits >= token.size()) {
                        return false;
                    }
                    std::string suffix = token.substr(digits);
                    if (auto redirect = parse_redirect_operator(suffix)) {
                        return redirect_requires_operand(*redirect);
                    }
                    return false;
                };

                QuoteInfo last_token(merged_tokens.back());
                if (last_token.is_unquoted() && requires_operand(last_token.value)) {
                    std::vector<std::string> suggestions = {
                        "Provide a destination after the redirection operator."};
                    append_context_hint(suggestions, text, current_line_number);
                    return report_error_with_code(ErrorType::SYNTAX_ERROR, ErrorSeverity::ERROR, "",
                                                  "syntax error near unexpected token `newline'",
                                                  suggestions, 2);
                }
            }

            cmds = shell_parser->parse_pipeline_with_preprocessing(text);

            has_multiple_commands = cmds.size() > 1;
            has_redir_or_pipe = has_multiple_commands;
            if (!has_multiple_commands && !cmds.empty()) {
                const auto& c = cmds[0];
                has_redir_or_pipe =
                    c.background || !c.input_file.empty() || !c.output_file.empty() ||
                    !c.append_file.empty() || c.stderr_to_stdout || c.stdout_to_stderr ||
                    !c.stderr_file.empty() || !c.here_doc.empty() || c.both_output ||
                    !c.here_string.empty() || !c.fd_redirections.empty() ||
                    !c.fd_duplications.empty() || !c.redirection_order.empty();

                if (c.negate_pipeline) {
                    has_redir_or_pipe = true;
                }
            }

            if (has_multiple_commands) {
                bool looks_like_case =
                    !trimmed_text.empty() &&
                    is_statement_keyword_prefix(trimmed_text, StatementKeyword::Case);
                if (looks_like_case) {
                    has_multiple_commands = false;
                    has_redir_or_pipe = false;
                    cmds.clear();
                }
            }

            if (!has_multiple_commands) {
                // Pipeline parsing already expanded these words. Re-parsing can repeat
                // arithmetic assignments and other expansion side effects.
                if (!cmds.empty()) {
                    parsed_args = cmds.front().args;
                }

                if (!parsed_args.empty()) {
                    const std::string& prog = parsed_args[0];
                    if (should_interpret_as_cjsh_script(prog)) {
                        std::ifstream f(prog);
                        if (!f) {
                            print_error({ErrorType::RUNTIME_ERROR,
                                         "",
                                         "Failed to open script file: " + prog,
                                         {}});
                            return 1;
                        }
                        std::stringstream buffer;
                        buffer << f.rdbuf();
                        auto nested_lines = shell_parser->parse_into_lines(buffer.str());
                        return execute_block(nested_lines);
                    }

                    auto stmt_keyword = parse_statement_keyword_prefix(prog);
                    if (stmt_keyword.has_value() && *stmt_keyword != StatementKeyword::Case) {
                        std::vector<std::string> block_lines;
                        if (shell_parser) {
                            block_lines = shell_parser->parse_into_lines(text);
                        }
                        if (block_lines.empty()) {
                            block_lines.push_back(text);
                        }

                        auto run_block = [&]() -> int { return execute_block(block_lines); };
                        int exit_code = 0;
                        bool handled_with_redirections = false;

                        if (g_shell && g_shell->shell_exec) {
                            try {
                                std::vector<Command> control_cmds =
                                    shell_parser->parse_pipeline_with_preprocessing(text);
                                if (!control_cmds.empty()) {
                                    const Command& control_cmd = control_cmds[0];
                                    std::string command_name =
                                        control_cmd.args.empty() ? prog : control_cmd.args[0];
                                    bool action_invoked = false;
                                    exit_code = g_shell->shell_exec->run_with_command_redirections(
                                        control_cmd, run_block, command_name, false,
                                        &action_invoked);
                                    if (!action_invoked) {
                                        return exit_code;
                                    }
                                    handled_with_redirections = true;
                                }
                            } catch (const std::exception&) {
                                // Best-effort parse; fall back to normal execution.
                            }
                        }

                        if (!handled_with_redirections) {
                            exit_code = run_block();
                        }

                        return exit_code;
                    }
                }
            }
        } catch (const std::runtime_error& e) {
            return handle_runtime_exception(text, e, current_line_number);
        }

        if (!has_multiple_commands && is_statement_keyword_prefix(text, StatementKeyword::Case) &&
            text.find("esac") == std::string::npos) {
            std::string completed_case = text + ";; esac";
            return execute_simple_or_pipeline(completed_case);
        }

        if (!has_multiple_commands) {
            if (auto inline_case_result = try_handle_inline_case(text, false)) {
                return *inline_case_result;
            }
        }

        try {
            if (!has_redir_or_pipe && !cmds.empty()) {
                const auto& c = cmds[0];

                if (!c.args.empty() && c.args[0] == "__INTERNAL_SUBSHELL__") {
                    if (command_has_redirection(c)) {
                        return run_pipeline(cmds);
                    }
                    if (c.args.size() >= 2) {
                        return execute_subshell(c.args[1]);
                    } else {
                        return 1;
                    }

                } else if (!c.args.empty() && c.args[0] == "__INTERNAL_BRACE_GROUP__") {
                    if (command_has_redirection(c)) {
                        return run_pipeline(cmds);
                    }

                    if (c.args.size() >= 2) {
                        int exit_code = g_shell ? g_shell->execute(c.args[1]) : 1;
                        return set_last_status(exit_code);
                    }

                    return 0;

                } else {
                    std::vector<std::string> expanded_args = std::move(parsed_args);
                    const bool is_alias_pipeline =
                        expanded_args.size() == 2 && expanded_args[0] == "__ALIAS_PIPELINE__";
                    if (!c.args.empty()) {
                        if (expanded_args.empty()) {
                            expanded_args = c.args;
                        } else if (c.auto_background_on_stop && !is_alias_pipeline) {
                            expanded_args = c.args;
                        }
                    }
                    if (expanded_args.empty()) {
                        return 0;
                    }

                    if (expanded_args.size() == 2 && expanded_args[0] == "__ALIAS_PIPELINE__") {
                        std::string pipeline_text = expanded_args[1];
                        if (c.auto_background_on_stop) {
                            pipeline_text += c.auto_background_on_stop_silent ? " &^!" : " &^";
                        }
                        std::vector<Command> pipeline_cmds =
                            shell_parser->parse_pipeline_with_preprocessing(pipeline_text);
                        return run_pipeline(pipeline_cmds);
                    }

                    int env_result = handle_env_assignment(expanded_args);
                    if (env_result >= 0) {
                        return env_result;
                    }

                    if (!expanded_args.empty() && functions.count(expanded_args[0])) {
                        return call_function(expanded_args);
                    }

                    std::vector<std::pair<std::string, std::string>> function_assignments;
                    size_t function_index =
                        cjsh_env::collect_env_assignments(expanded_args, function_assignments);
                    if (function_index > 0 && function_index < expanded_args.size() &&
                        functions.count(expanded_args[function_index]) != 0U) {
                        return run_pipeline(cmds);
                    }
                    int exit_code = g_shell->execute_command(expanded_args, c.background,
                                                             c.auto_background_on_stop,
                                                             c.auto_background_on_stop_silent);
                    return set_last_status(exit_code);
                }
            }

            if (cmds.empty()) {
                return 0;
            }
            return run_pipeline(cmds);
        } catch (const std::bad_alloc&) {
            std::vector<std::string> suggestions = {
                "Command may be too complex or system is low on memory."};
            append_context_hint(suggestions, text, current_line_number);
            return report_error_with_code(ErrorType::RUNTIME_ERROR, ErrorSeverity::ERROR,
                                          "interpreter", "memory allocation failed", suggestions,
                                          3);
        } catch (const std::system_error& e) {
            std::vector<std::string> suggestions = {"Check system resources and permissions."};
            append_context_hint(suggestions, text, current_line_number);
            return report_error_with_code(ErrorType::RUNTIME_ERROR, ErrorSeverity::ERROR,
                                          "interpreter", strip_cjsh_prefix(e.what()), suggestions,
                                          4);
        } catch (const std::runtime_error& e) {
            return handle_runtime_exception(text, e, current_line_number);
        } catch (const std::exception& e) {
            std::vector<std::string> suggestions = {
                "Please report this issue along with steps to reproduce."};
            append_context_hint(suggestions, text, current_line_number);
            return report_error_with_code(ErrorType::UNKNOWN_ERROR, ErrorSeverity::ERROR,
                                          "interpreter", strip_cjsh_prefix(e.what()), suggestions,
                                          5);
        } catch (...) {
            std::vector<std::string> suggestions = {
                "Please report this issue along with steps to reproduce."};
            append_context_hint(suggestions, text, current_line_number);
            return report_error_with_code(ErrorType::UNKNOWN_ERROR, ErrorSeverity::ERROR,
                                          "interpreter", "unknown interpreter error", suggestions,
                                          6);
        }
    };

    try_handle_inline_case = [this, &execute_simple_or_pipeline](
                                 const std::string& candidate,
                                 bool allow_command_substitution) -> std::optional<int> {
        auto pattern_match_fn = [this](const std::string& text, const std::string& pattern) {
            return pattern_matcher.matches_pattern(text, pattern, true);
        };
        auto cmd_sub_expander = [this, &execute_simple_or_pipeline](const std::string& input) {
            std::string expanded = expand_all_substitutions(input, execute_simple_or_pipeline);
            return std::make_pair(expanded, std::vector<std::string>{});
        };
        return case_evaluator::handle_inline_case(candidate, execute_simple_or_pipeline,
                                                  allow_command_substitution, true, shell_parser,
                                                  pattern_match_fn, cmd_sub_expander);
    };

    auto check_pending_signals = [&]() -> std::optional<int> {
        return collect_pending_signal_exit_code();
    };

    auto should_abort_for_parameter_expansion = [] { return g_parameter_expansion_fatal_error; };

    int last_code = 0;

    auto execute_block_wrapper = [&](const std::vector<std::string>& block_lines) -> int {
        return execute_block(block_lines);
    };

    auto execute_block_skip_validation = [&](const std::vector<std::string>& block_lines) -> int {
        // loop bodies re-enter execute_block through this wrapper so continue and break validity
        // checks can use loop scope while skipping redundant parent-level syntax validation
        push_loop_scope();
        auto loop_scope_cleanup = [this](void*) { pop_loop_scope(); };
        auto loop_scope =
            std::unique_ptr<void, decltype(loop_scope_cleanup)>(nullptr, loop_scope_cleanup);

        return execute_block(block_lines, true);
    };

    // central if evaluator used by both multiline script flow and one-line inline conditionals
    auto handle_if_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        return conditional_evaluator::handle_if_block(
            src_lines, idx, execute_block_wrapper, execute_simple_or_pipeline,
            evaluate_logical_condition, shell_parser, should_abort_for_parameter_expansion);
    };

    auto handle_for_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        // for blocks route to loop_evaluator where header parsing and per-iteration body execution
        // are handled for list range and c-style forms
        return loop_evaluator::handle_for_block(
            src_lines, idx, execute_block_skip_validation,
            [this](const std::string& expr) { return evaluate_arithmetic_expression(expr); },
            execute_simple_or_pipeline, shell_parser, should_abort_for_parameter_expansion);
    };

    auto handle_select_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        return loop_evaluator::handle_select_block(src_lines, idx, execute_block_skip_validation,
                                                   execute_simple_or_pipeline, shell_parser,
                                                   should_abort_for_parameter_expansion);
    };

    auto handle_case_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        std::string first = trim(strip_inline_comment(src_lines[idx]));
        if (!is_statement_keyword_prefix(first, StatementKeyword::Case)) {
            return 1;
        }

        if (auto inline_case_result = try_handle_inline_case(first, true)) {
            return *inline_case_result;
        }

        std::string header_accum = first;
        size_t j = idx;
        bool found_in = false;

        auto header_tokens = shell_parser->parse_command(header_accum);
        if (std::find(header_tokens.begin(), header_tokens.end(), "in") != header_tokens.end()) {
            found_in = true;
        }

        while (!found_in && ++j < src_lines.size()) {
            std::string cur = trim(strip_inline_comment(src_lines[j]));
            if (cur.empty()) {
                continue;
            }
            header_accum += " " + cur;
            header_tokens = shell_parser->parse_command(header_accum);
            if (std::find(header_tokens.begin(), header_tokens.end(), "in") !=
                header_tokens.end()) {
                found_in = true;
                break;
            }
        }

        if (!found_in) {
            idx = j;
            return 1;
        }

        std::string expanded_header = header_accum;
        if (header_accum.find("$(") != std::string::npos) {
            expanded_header = expand_all_substitutions(header_accum, execute_simple_or_pipeline);
        }

        size_t in_pos = expanded_header.find(" in ");
        if (in_pos == std::string::npos && expanded_header.length() >= 3 &&
            expanded_header.compare(expanded_header.length() - 3, 3, " in") == 0) {
            in_pos = expanded_header.length() - 3;
        }
        if (in_pos == std::string::npos) {
            idx = j;
            return 1;
        }

        std::string case_part = expanded_header.substr(0, in_pos);
        std::string raw_case_value;
        size_t case_space_pos = case_part.find(' ');
        if (case_space_pos != std::string::npos &&
            is_statement_keyword_prefix(case_part.substr(0, case_space_pos),
                                        StatementKeyword::Case)) {
            raw_case_value = trim(case_part.substr(case_space_pos + 1));
        }

        if (raw_case_value.empty()) {
            idx = j;
            return 1;
        }

        std::string case_value = case_evaluator::normalize_case_value(raw_case_value, shell_parser);

        std::string inline_segment;
        if (expanded_header.length() >= in_pos + 4) {
            inline_segment = trim(expanded_header.substr(in_pos + 4));
        }

        size_t esac_index = j;
        bool inline_has_esac = false;
        size_t inline_esac_pos = parser_find_keyword_token(inline_segment, "esac");
        if (inline_esac_pos != std::string::npos) {
            inline_has_esac = true;
            inline_segment = trim(inline_segment.substr(0, inline_esac_pos));
        }

        std::string combined_patterns;
        if (!inline_segment.empty()) {
            combined_patterns = inline_segment;
        }

        if (!inline_has_esac) {
            auto body_pair = case_evaluator::collect_case_body(src_lines, j + 1);
            std::string body_content = body_pair.first;
            esac_index = body_pair.second;
            if (esac_index >= src_lines.size()) {
                idx = esac_index;
                return 1;
            }
            if (!body_content.empty()) {
                if (!combined_patterns.empty()) {
                    combined_patterns += '\n';
                }
                combined_patterns += body_content;
            }
        } else {
            esac_index = j;
        }

        auto case_pattern_match_fn = [this](const std::string& text, const std::string& pattern) {
            return pattern_matcher.matches_pattern(text, pattern, true);
        };

        auto case_result = case_evaluator::evaluate_case_patterns(
            combined_patterns, case_value, false, execute_simple_or_pipeline, shell_parser,
            case_pattern_match_fn);
        idx = esac_index;
        return case_result.first ? case_result.second : 0;
    };

    auto handle_while_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        // while blocks use the shared condition loop evaluator
        return loop_evaluator::handle_condition_loop_block(
            loop_evaluator::LoopCondition::WHILE, src_lines, idx, execute_block_skip_validation,
            execute_simple_or_pipeline, shell_parser, should_abort_for_parameter_expansion);
    };

    auto handle_until_block = [&](const std::vector<std::string>& src_lines, size_t& idx) -> int {
        // until blocks use the same evaluator with inverted continuation condition
        return loop_evaluator::handle_condition_loop_block(
            loop_evaluator::LoopCondition::UNTIL, src_lines, idx, execute_block_skip_validation,
            execute_simple_or_pipeline, shell_parser, should_abort_for_parameter_expansion);
    };

    // runtime execution trace starts here after parser and validation stages have completed
    for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
        current_line_number = line_index + 1;

        if (auto pending_code = check_pending_signals()) {
            last_code = *pending_code;
            return set_last_status(last_code);
        }

        const auto& raw_line = lines[line_index];
        std::string line = trim(strip_inline_comment(raw_line));

        if (line.empty()) {
            continue;
        }

        if (should_skip_line(line)) {
            if (g_shell != nullptr && g_shell->get_shell_option(ShellOption::Verbose)) {
                std::cerr << line << '\n';
            }
            continue;
        }

        // first dispatch pass for structured control-flow blocks.
        // if for select while until and case are routed before generic command parsing.
        auto block_result = try_dispatch_block_statement(
            lines, line_index, line, handle_if_block, handle_for_block, handle_select_block,
            handle_while_block, handle_until_block, handle_case_block);

        if (block_result.handled) {
            last_code = block_result.exit_code;
            line_index = block_result.next_line_index;
            if (is_terminating_signal_exit_code(last_code)) {
                return set_last_status(last_code);
            }
            if (g_parameter_expansion_fatal_error) {
                return set_last_status(last_code);
            }
            if (is_control_flow_exit_code(last_code) || cjsh_env::exit_requested()) {
                return last_code;
            }
            (void)set_last_status(last_code);
            continue;
        }

        if (function_evaluator::parse_function_header(line, true)) {
            auto parse_result = function_evaluator::parse_and_register_functions(
                line, lines, line_index, functions, trim, strip_inline_comment,
                [this](const std::string& body) { return shell_parser->parse_into_lines(body); });

            if (!parse_result.remaining_line.empty()) {
                line = parse_result.remaining_line;
            } else {
                continue;
            }
        }

        // detect loop keywords on the right side of a pipeline and execute the full loop block
        // as a single combined command so done matching stays intact
        bool handled_pipeline_loop = false;
        size_t pipe_search_pos = 0;
        while (pipe_search_pos < line.size() && line.front() != '{' && line.front() != '(') {
            size_t pipe_pos = line.find('|', pipe_search_pos);
            if (pipe_pos == std::string::npos) {
                break;
            }

            if (is_char_escaped(line, pipe_pos) || is_inside_quotes(line, pipe_pos)) {
                pipe_search_pos = pipe_pos + 1;
                continue;
            }
            std::string after_pipe = trim(line.substr(pipe_pos + 1));
            auto loop_keyword = parse_statement_keyword_prefix(after_pipe);
            bool is_loop_keyword_prefix =
                loop_keyword.has_value() && is_loop_keyword(*loop_keyword);

            if (!is_loop_keyword_prefix) {
                pipe_search_pos = pipe_pos + 1;
                continue;
            }

            size_t gather_index = line_index;
            int nested_loop_depth = 0;
            std::vector<std::string> block_lines;
            block_lines.reserve(4);

            while (gather_index < lines.size()) {
                const std::string& gather_raw = lines[gather_index];
                std::string gather_trimmed = trim(strip_inline_comment(gather_raw));

                block_lines.push_back(gather_raw);

                if (contains_token(gather_trimmed, "do")) {
                    nested_loop_depth++;
                }
                if (contains_token(gather_trimmed, "done")) {
                    nested_loop_depth--;
                    if (nested_loop_depth <= 0) {
                        break;
                    }
                }

                gather_index++;
            }

            if (nested_loop_depth <= 0 && !block_lines.empty()) {
                std::string combined;
                combined.reserve(128);
                for (size_t idx = 0; idx < block_lines.size(); ++idx) {
                    if (idx > 0) {
                        combined.push_back('\n');
                    }
                    combined += block_lines[idx];
                }

                last_code = execute_simple_or_pipeline(combined);
                line_index = gather_index;
                handled_pipeline_loop = true;
            }

            break;
        }

        if (handled_pipeline_loop) {
            continue;
        }

        std::vector<LogicalCommand> lcmds = shell_parser->parse_logical_commands(line);
        if (lcmds.empty()) {
            continue;
        }

        last_code = 0;
        for (size_t i = 0; i < lcmds.size(); ++i) {
            const auto& lc = lcmds[i];

            if (i > 0) {
                const std::string& prev_op = lcmds[i - 1].op;

                bool is_control_flow = is_control_flow_exit_code(last_code);
                auto op = parse_logical_operator(prev_op);
                if (op.has_value()) {
                    if (*op == LogicalOperator::And && last_code != 0 && !is_control_flow) {
                        continue;
                    }
                    if (*op == LogicalOperator::Or && last_code == 0) {
                        continue;
                    }
                }

                if (is_control_flow) {
                    break;
                }
            }

            std::string cmd_to_parse = lc.command;
            std::string trimmed_cmd = trim(strip_inline_comment(cmd_to_parse));

            if (!trimmed_cmd.empty() && (trimmed_cmd[0] == '(' || trimmed_cmd[0] == '{')) {
                int code = execute_simple_or_pipeline(cmd_to_parse);
                last_code = code;
                continue;
            }

            if (is_statement_keyword_prefix(trimmed_cmd, StatementKeyword::If) &&
                (trimmed_cmd.find("; then") != std::string::npos) &&
                (trimmed_cmd.find(" fi") != std::string::npos ||
                 trimmed_cmd.find("; fi") != std::string::npos ||
                 trimmed_cmd.rfind("fi") == trimmed_cmd.length() - 2)) {
                // fast path for one-line if forms inside logical command chains
                size_t local_idx = 0;
                std::vector<std::string> one{trimmed_cmd};
                int code = handle_if_block(one, local_idx);
                last_code = code;
                if (g_parameter_expansion_fatal_error) {
                    return set_last_status(last_code);
                }
                continue;
            }

            if (is_statement_keyword_prefix(trimmed_cmd, StatementKeyword::Case) &&
                (trimmed_cmd.find(" in ") != std::string::npos) &&
                (trimmed_cmd.find("esac") != std::string::npos)) {
                size_t local_idx = 0;
                std::vector<std::string> one{trimmed_cmd};
                int code = handle_case_block(one, local_idx);
                last_code = code;
                if (g_parameter_expansion_fatal_error) {
                    return set_last_status(last_code);
                }
                continue;
            }

            auto semis = shell_parser->parse_semicolon_commands(lc.command);
            if (semis.empty()) {
                last_code = 0;
                continue;
            }
            for (size_t k = 0; k < semis.size(); ++k) {
                const std::string& semi = semis[k];
                auto segs = shell_script_interpreter::detail::split_ampersand(semi);
                if (segs.empty()) {
                    segs.push_back(semi);
                }
                for (const auto& cmd_text : segs) {
                    if (g_shell != nullptr && g_shell->get_shell_option(ShellOption::Verbose)) {
                        std::string verbose_text = trim(strip_inline_comment(cmd_text));
                        if (!verbose_text.empty()) {
                            std::cerr << verbose_text << '\n';
                        }
                    }

                    std::string t = trim(strip_inline_comment(cmd_text));

                    const auto function_header = function_evaluator::parse_function_header(t);
                    if (function_header) {
                        const auto& func_name = function_header->name;
                        const size_t body_start_pos = function_header->body_start;
                        const char opening_delim = function_header->opening;
                        std::vector<std::string> body_lines;
                        std::string after_body_open = trim(t.substr(body_start_pos + 1));
                        if (!after_body_open.empty()) {
                            size_t body_close_pos = opening_delim == '{'
                                                        ? find_matching_brace(t, body_start_pos)
                                                        : find_matching_paren(t, body_start_pos);
                            if (body_close_pos != std::string::npos) {
                                std::string body_part = trim(t.substr(
                                    body_start_pos + 1, body_close_pos - body_start_pos - 1));
                                body_lines = shell_parser->parse_into_lines(body_part);
                                if (readonly_function_manager_is(func_name)) {
                                    print_error({ErrorType::INVALID_ARGUMENT,
                                                 "readonly",
                                                 func_name + ": readonly function",
                                                 {}});
                                    last_code = 1;
                                } else {
                                    functions[func_name] = {body_lines, opening_delim == '('};
                                    last_code = 0;
                                }
                                (void)set_last_status(last_code);
                                continue;
                            }
                        }
                    }

                    if (is_statement_keyword_prefix(t, StatementKeyword::For) &&
                        t.find("; do") != std::string::npos) {
                        // one-line for loop with explicit do terminator in the same segment
                        size_t local_idx = 0;
                        std::vector<std::string> one{t};
                        int code = handle_for_block(one, local_idx);
                        last_code = code;
                        if (g_parameter_expansion_fatal_error) {
                            return set_last_status(last_code);
                        }
                        continue;
                    }
                    if (is_statement_keyword_prefix(t, StatementKeyword::Select) &&
                        t.find("; do") != std::string::npos) {
                        size_t local_idx = 0;
                        std::vector<std::string> one{t};
                        int code = handle_select_block(one, local_idx);
                        last_code = code;
                        if (g_parameter_expansion_fatal_error) {
                            return set_last_status(last_code);
                        }
                        continue;
                    }
                    if (is_statement_keyword_prefix(t, StatementKeyword::While) &&
                        t.find("; do") != std::string::npos) {
                        // one-line while loop with explicit do terminator in the same segment
                        size_t local_idx = 0;
                        std::vector<std::string> one{t};
                        int code = handle_while_block(one, local_idx);
                        last_code = code;
                        if (g_parameter_expansion_fatal_error) {
                            return set_last_status(last_code);
                        }
                        continue;
                    }
                    if (is_statement_keyword_prefix(t, StatementKeyword::Until) &&
                        t.find("; do") != std::string::npos) {
                        // one-line until loop with explicit do terminator in the same segment
                        size_t local_idx = 0;
                        std::vector<std::string> one{t};
                        int code = handle_until_block(one, local_idx);
                        last_code = code;
                        if (g_parameter_expansion_fatal_error) {
                            return set_last_status(last_code);
                        }
                        continue;
                    }

                    if (is_statement_keyword_prefix(t, StatementKeyword::If) &&
                        t.find("; then") != std::string::npos &&
                        t.find(" fi") != std::string::npos) {
                        // same one-line if handoff while iterating semicolon-split command segments
                        size_t local_idx = 0;
                        std::vector<std::string> one{t};
                        int code = handle_if_block(one, local_idx);
                        last_code = code;
                        if (g_parameter_expansion_fatal_error) {
                            return set_last_status(last_code);
                        }
                        continue;
                    }

                    if (is_statement_keyword_prefix(t, StatementKeyword::For)) {
                        // multiline inline-do form split by semicolons is reconstructed here
                        if (auto inline_result = loop_evaluator::try_execute_inline_do_block(
                                t, semis, k, handle_for_block)) {
                            last_code = *inline_result;
                            if (g_parameter_expansion_fatal_error) {
                                return set_last_status(last_code);
                            }
                            break;
                        }
                    }
                    if (is_statement_keyword_prefix(t, StatementKeyword::Select)) {
                        if (auto inline_result = loop_evaluator::try_execute_inline_do_block(
                                t, semis, k, handle_select_block)) {
                            last_code = *inline_result;
                            if (g_parameter_expansion_fatal_error) {
                                return set_last_status(last_code);
                            }
                            break;
                        }
                    }
                    if (is_statement_keyword_prefix(t, StatementKeyword::While)) {
                        // same reconstruction path for while loops missing inline ; do in this
                        // piece
                        if (auto inline_result = loop_evaluator::try_execute_inline_do_block(
                                t, semis, k, handle_while_block)) {
                            last_code = *inline_result;
                            if (g_parameter_expansion_fatal_error) {
                                return set_last_status(last_code);
                            }
                            break;
                        }
                    }

                    if (is_statement_keyword_prefix(t, StatementKeyword::Until)) {
                        // same reconstruction path for until loops missing inline ; do in this
                        // piece
                        if (auto inline_result = loop_evaluator::try_execute_inline_do_block(
                                t, semis, k, handle_until_block)) {
                            last_code = *inline_result;
                            if (g_parameter_expansion_fatal_error) {
                                return set_last_status(last_code);
                            }
                            break;
                        }
                    }

                    int code = 0;
                    bool is_function_call = false;
                    try {
                        code = execute_simple_or_pipeline_impl(cmd_text, true, &is_function_call);
                    } catch (const std::runtime_error&) {
                        code = 1;
                    }
                    last_code = code;

                    (void)set_last_status(last_code);

                    if (g_parameter_expansion_fatal_error) {
                        return set_last_status(last_code);
                    }

                    if (is_terminating_signal_exit_code(code)) {
                        return set_last_status(code);
                    }

                    if (auto pending_code = check_pending_signals()) {
                        last_code = *pending_code;
                        return set_last_status(last_code);
                    }

                    const bool is_nonfinal_logical_command = !lc.op.empty();
                    if (g_shell && g_shell->should_abort_on_nonzero_exit(code) && code != 0 &&
                        !is_nonfinal_logical_command) {
                        if (code != 253 && code != 254 && code != 255) {
                            return code;
                        }
                    }

                    if (!is_function_call && is_control_flow_exit_code(code)) {
                        bool control_flow_error = false;
                        if (code == exit_return && !in_function_scope() && !in_source_scope()) {
                            print_error({ErrorType::INVALID_ARGUMENT,
                                         "return",
                                         "return outside function",
                                         {}});
                            control_flow_error = true;
                        } else if (code == 254 && !in_loop_scope()) {
                            print_error({ErrorType::INVALID_ARGUMENT,
                                         "continue",
                                         "continue outside loop",
                                         {}});
                            control_flow_error = true;
                        } else if (code == 255 && !in_loop_scope()) {
                            print_error(
                                {ErrorType::INVALID_ARGUMENT, "break", "break outside loop", {}});
                            control_flow_error = true;
                        }

                        if (control_flow_error) {
                            code = 1;
                            last_code = code;
                            (void)set_last_status(last_code);
                        } else {
                            goto control_flow_exit;
                        }
                    }
                }
            }
        }

    control_flow_exit:

        if (is_terminating_signal_exit_code(last_code)) {
            return set_last_status(last_code);
        }

        if (last_code == exit_command_not_found) {
            if (g_shell && g_shell->should_abort_on_nonzero_exit(last_code)) {
                return last_code;
            }
        } else if (is_control_flow_exit_code(last_code)) {
            return last_code;
        } else if (last_code != 0) {
            continue;
        }
    }

    return last_code;
}

bool ShellScriptInterpreter::should_interpret_as_cjsh_script(const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    const bool has_explicit_path = path.find('/') != std::string::npos;
    std::filesystem::path candidate(path);
    const std::string extension = string_utils::to_lower_copy(candidate.extension().string());

    if (!has_explicit_path && extension != ".cjsh") {
        return false;
    }

    if (!is_readable_file(path)) {
        return false;
    }

    if (extension == ".cjsh") {
        return true;
    }

    std::ifstream f(path);
    // Most explicit paths name binaries. Check the shebang before reading a line,
    // which may otherwise consume an entire binary with no newline.
    if (!f || f.get() != '#' || f.get() != '!') {
        return false;
    }
    std::string first_line;
    (void)std::getline(f, first_line);
    return first_line.find("cjsh") != std::string::npos;
}

int ShellScriptInterpreter::evaluate_logical_condition_internal(
    const std::string& condition, cjsh::FunctionRef<int(const std::string&)> executor) {
    // this is the interpreter-side condition pipeline used by if and elif before branch selection
    std::string cond = trim(condition);
    if (cond.empty()) {
        return 1;
    }

    // resolve arithmetic command substitutions first so the lower condition evaluator receives
    // final text with numeric results in place
    std::string processed_cond = cond;
    size_t pos = 0;
    while ((pos = processed_cond.find("$((", pos)) != std::string::npos) {
        size_t start = pos + 3;
        size_t depth = 1;
        size_t end = start;

        while (end < processed_cond.length() && depth > 0) {
            if (end + 1 < processed_cond.length() && processed_cond.substr(end, 2) == "((") {
                depth++;
                end += 2;
            } else if (end + 1 < processed_cond.length() && processed_cond.substr(end, 2) == "))") {
                depth--;
                if (depth == 0) {
                    break;
                }
                end += 2;
            } else {
                end++;
            }
        }

        if (depth == 0 && end + 1 < processed_cond.length()) {
            std::string expr = processed_cond.substr(start, end - start);

            if (shell_parser != nullptr) {
                shell_parser->expand_env_vars(expr);
            }

            try {
                long long result = evaluate_arithmetic_expression(expr);
                std::string result_str = std::to_string(result);

                std::string new_cond;
                new_cond.reserve(processed_cond.size() - (end + 2 - pos) + result_str.size());
                (void)new_cond.append(processed_cond, 0, pos);
                (void)new_cond.append(result_str);
                (void)new_cond.append(processed_cond, end + 2, std::string::npos);
                processed_cond = std::move(new_cond);
                pos = pos + result_str.length();
            } catch (const std::exception&) {
                pos = end + 2;
            }
        } else {
            pos++;
        }
    }
    // final pass does logical tokenization and short-circuit execution over && and ||
    int condition_status =
        conditional_evaluator::evaluate_logical_condition(processed_cond, executor);

    (void)set_last_status(condition_status);
    return condition_status;
}

long long ShellScriptInterpreter::evaluate_arithmetic_expression(const std::string& expr) {
    auto var_reader = [this](const std::string& name) -> long long {
        std::string var_value = variable_manager.get_variable_value(name);
        if (var_value.empty()) {
            return 0;
        }

        char* end = nullptr;
        long long parsed_value = std::strtoll(var_value.c_str(), &end, 0);
        if (end == var_value.c_str()) {
            return 0;
        }

        return parsed_value;
    };

    auto var_writer = [this](const std::string& name, long long value) {
        std::string value_str = std::to_string(value);

        if (variable_manager.is_local_variable(name)) {
            variable_manager.set_local_variable(name, value_str);
            return;
        }

        variable_manager.set_environment_variable(name, value_str);
    };

    ArithmeticEvaluator evaluator(var_reader, var_writer);
    return evaluator.evaluate(expr);
}

int ShellScriptInterpreter::set_last_status(int code) {
    Exec* exec_ptr = (g_shell && g_shell->shell_exec) ? g_shell->shell_exec.get() : nullptr;
    pipeline_status_utils::apply_execution_status_env(code, exec_ptr);

    return code;
}

int ShellScriptInterpreter::run_pipeline(const std::vector<Command>& cmds) {
    if (!g_shell || !g_shell->shell_exec) {
        return set_last_status(1);
    }

    int exit_code = g_shell->shell_exec->execute_pipeline(cmds);
    g_shell->shell_exec->print_error_if_needed(exit_code);
    return set_last_status(exit_code);
}

std::string ShellScriptInterpreter::expand_parameter_expression(const std::string& param_expr) {
    auto var_reader = [this](const std::string& name) -> std::string {
        return variable_manager.get_variable_value(name);
    };

    auto var_writer = [this](const std::string& name, const std::string& value) {
        if (!readonly_manager_can_assign(name, "parameter expansion")) {
            return;
        }

        (void)variable_manager.assign_variable(name, value, false);
    };

    auto var_checker = [this](const std::string& name) -> bool {
        return variable_manager.variable_is_set(name);
    };

    auto pattern_match_fn = [this](const std::string& text, const std::string& pattern) -> bool {
        return pattern_matcher.matches_pattern(text, pattern);
    };

    auto array_length_reader = [this](const std::string& name) -> std::optional<size_t> {
        if (name == "@" || name == "*") {
            return flags::get_positional_parameter_count();
        }
        return variable_manager.get_array_length(name);
    };

    auto array_keys_reader = [this](const std::string& name) -> std::string {
        return variable_manager.get_array_keys(name);
    };

    auto word_expander = [this](const std::string& word) -> std::string {
        std::string expanded = expand_all_substitutions(word, [](const std::string& command) {
            return g_shell ? g_shell->execute(command) : 1;
        });

        if (!expanded.empty() && expanded[0] == '~' &&
            (expanded.size() == 1 || expanded[1] == '/')) {
            std::string home = get_variable_value("HOME");
            if (!home.empty()) {
                expanded = home + expanded.substr(1);
            }
        }
        return expanded;
    };

    auto indirect_reader = [this](const std::string& name) -> std::string {
        return variable_manager.get_indirect_value(name);
    };

    ParameterExpansionEvaluator evaluator(
        var_reader, var_writer, var_checker, pattern_match_fn, array_length_reader,
        array_keys_reader, word_expander, indirect_reader,
        [this](const std::string& text, const std::string& pattern, bool longest) {
            return pattern_matcher.match_end_positions(text, pattern, longest);
        });
    return evaluator.expand(param_expr);
}

std::string ShellScriptInterpreter::get_variable_value(const std::string& var_name) {
    return variable_manager.get_variable_value(var_name);
}

bool ShellScriptInterpreter::variable_is_set(const std::string& var_name) {
    return variable_manager.variable_is_set(var_name);
}

bool ShellScriptInterpreter::has_function(const std::string& name) const {
    return function_evaluator::has_function(functions, name);
}

std::vector<std::string> ShellScriptInterpreter::get_function_names() const {
    return function_evaluator::get_function_names(functions);
}

void ShellScriptInterpreter::push_function_scope() {
    variable_manager.push_scope();
}

void ShellScriptInterpreter::pop_function_scope() {
    variable_manager.pop_scope();
}

void ShellScriptInterpreter::set_local_variable(const std::string& name, const std::string& value) {
    variable_manager.set_local_variable(name, value);
}

bool ShellScriptInterpreter::is_local_variable(const std::string& name) const {
    return variable_manager.is_local_variable(name);
}

bool ShellScriptInterpreter::unset_local_variable(const std::string& name) {
    return variable_manager.unset_local_variable(name);
}

void ShellScriptInterpreter::mark_local_as_exported(const std::string& name) {
    variable_manager.mark_local_as_exported(name);
}

bool ShellScriptInterpreter::in_function_scope() const {
    return variable_manager.in_function_scope();
}

void ShellScriptInterpreter::push_source_scope() {
    ++source_depth;
}

void ShellScriptInterpreter::pop_source_scope() {
    if (source_depth > 0) {
        --source_depth;
    }
}

bool ShellScriptInterpreter::in_source_scope() const {
    return source_depth > 0;
}

void ShellScriptInterpreter::push_loop_scope() {
    ++loop_depth;
}

void ShellScriptInterpreter::pop_loop_scope() {
    if (loop_depth > 0) {
        --loop_depth;
    }
}

bool ShellScriptInterpreter::in_loop_scope() const {
    return loop_depth > 0;
}

ShellScriptInterpreter::BlockHandlerResult ShellScriptInterpreter::try_dispatch_block_statement(
    const std::vector<std::string>& lines, size_t line_index, const std::string& line,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_if_block,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_for_block,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_select_block,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_while_block,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_until_block,
    cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_case_block) {
    // detect and route if blocks early so nested branches are handled as structured control flow
    if (line == "if" || line.rfind("if ", 0) == 0) {
        size_t idx = line_index;
        int rc = handle_if_block(lines, idx);
        return {true, rc, idx};
    }

    if (parser_starts_with_keyword_token(line, "for")) {
        // route for headers to loop_evaluator for header parsing and body collection through done
        size_t idx = line_index;
        int rc = handle_for_block(lines, idx);
        return {true, rc, idx};
    }

    if (parser_starts_with_keyword_token(line, "select")) {
        size_t idx = line_index;
        int rc = handle_select_block(lines, idx);
        return {true, rc, idx};
    }

    if (line == "while" || line.rfind("while ", 0) == 0) {
        // route while headers to the shared condition-loop path
        size_t idx = line_index;
        int rc = handle_while_block(lines, idx);
        return {true, rc, idx};
    }

    if (line == "until" || line.rfind("until ", 0) == 0) {
        // route until headers to the same condition-loop path with inverted continuation
        size_t idx = line_index;
        int rc = handle_until_block(lines, idx);
        return {true, rc, idx};
    }

    if (line == "case" || line.rfind("case ", 0) == 0) {
        size_t idx = line_index;
        int rc = handle_case_block(lines, idx);
        return {true, rc, idx};
    }

    return {false, 0, line_index};
}

std::string ShellScriptInterpreter::expand_all_substitutions(
    const std::string& input, cjsh::FunctionRef<int(const std::string&)> executor) {
    CommandSubstitutionEvaluator cmd_subst_evaluator(
        CommandSubstitutionEvaluator::create_command_executor(executor));

    auto expansion_result = cmd_subst_evaluator.expand_substitutions(input);
    if (!expansion_result.exit_codes.empty()) {
        last_substitution_exit_status = expansion_result.exit_codes.back();
        pending_assignment_exit_status = last_substitution_exit_status;
    } else {
        last_substitution_exit_status.reset();
    }
    std::string result = expansion_result.text;

    std::string out;
    out.reserve(result.size());

    bool in_quotes = false;
    char q = '\0';
    bool escaped = false;

    auto append_protected_substitution_output = [&](size_t& index, const std::string& start_marker,
                                                    const std::string& end_marker) {
        if (result.compare(index, start_marker.size(), start_marker) != 0) {
            return false;
        }

        size_t end = result.find(end_marker, index + start_marker.size());
        if (end == std::string::npos) {
            return false;
        }

        size_t end_after_marker = end + end_marker.size();
        (void)out.append(result, index, end_after_marker - index);
        index = end_after_marker - 1;
        return true;
    };

    for (size_t i = 0; i < result.size(); ++i) {
        if ((i & 255U) == 0U) {
            if (auto signal_exit = collect_pending_signal_exit_code()) {
                throw make_signal_exit_exception(*signal_exit);
            }
        }

        if (append_protected_substitution_output(i, noenv_start(), noenv_end()) ||
            append_protected_substitution_output(i, subst_literal_start(), subst_literal_end())) {
            continue;
        }

        char c = result[i];

        if (escaped) {
            out += '\\';
            out += c;
            escaped = false;
            continue;
        }

        if (c == '\\' && (!in_quotes || q != '\'')) {
            escaped = true;
            continue;
        }

        if ((c == '"' || c == '\'') && (!in_quotes)) {
            in_quotes = true;
            q = c;
            out += c;
            continue;
        }
        if (in_quotes && c == q) {
            in_quotes = false;
            q = '\0';
            out += c;
            continue;
        }

        if (!in_quotes || q == '"') {
            if (c == '$' && i + 2 < result.size() && result[i + 1] == '(' && result[i + 2] == '(') {
                size_t inner_start = i + 3;
                int depth = 1;
                size_t j = inner_start;
                bool found = false;

                for (; j < result.size(); ++j) {
                    if (j + 1 < result.size() && result[j] == '(' && result[j - 1] != '\\') {
                        depth++;
                    } else if (result[j] == ')' && (j == 0 || result[j - 1] != '\\')) {
                        depth--;
                        if (depth == 0 && j + 1 < result.size() && result[j + 1] == ')') {
                            found = true;
                            break;
                        }
                    }
                }

                if (found) {
                    size_t expr_len = (j > inner_start) ? (j - inner_start) : 0;
                    std::string expr = result.substr(inner_start, expr_len);

                    std::string expanded_expr;
                    for (size_t k = 0; k < expr.size(); ++k) {
                        if (expr[k] == '$' && k + 1 < expr.size()) {
                            if (isdigit(expr[k + 1])) {
                                std::string param_name(1, expr[k + 1]);
                                expanded_expr += get_variable_value(param_name);
                                k++;
                            } else if (isalpha(expr[k + 1]) || expr[k + 1] == '_') {
                                size_t var_start = k + 1;
                                size_t var_end = var_start;
                                while (var_end < expr.size() &&
                                       (isalnum(expr[var_end]) || expr[var_end] == '_')) {
                                    var_end++;
                                }
                                std::string var_name = expr.substr(var_start, var_end - var_start);
                                expanded_expr += get_variable_value(var_name);
                                k = var_end - 1;
                            } else if (expr[k + 1] == '{') {
                                size_t close_brace = expr.find('}', k + 2);
                                if (close_brace != std::string::npos) {
                                    std::string var_name =
                                        expr.substr(k + 2, close_brace - (k + 2));
                                    expanded_expr += get_variable_value(var_name);
                                    k = close_brace;
                                } else {
                                    expanded_expr += expr[k];
                                }
                            } else if (expr[k + 1] == '(' && k + 2 < expr.size() &&
                                       expr[k + 2] == '(') {
                                int nested_depth = 1;
                                size_t nested_start = k + 3;
                                size_t nested_end = nested_start;
                                for (; nested_end < expr.size(); ++nested_end) {
                                    if (expr[nested_end] == '(' &&
                                        (nested_end == 0 || expr[nested_end - 1] != '\\')) {
                                        nested_depth++;
                                    } else if (expr[nested_end] == ')' &&
                                               (nested_end == 0 || expr[nested_end - 1] != '\\')) {
                                        nested_depth--;
                                        if (nested_depth == 0 && nested_end + 1 < expr.size() &&
                                            expr[nested_end + 1] == ')') {
                                            std::string nested_expr = expr.substr(
                                                nested_start, nested_end - nested_start);
                                            try {
                                                expanded_expr += std::to_string(
                                                    evaluate_arithmetic_expression(nested_expr));
                                            } catch (...) {
                                                expanded_expr += '0';
                                            }
                                            k = nested_end + 1;
                                            break;
                                        }
                                    }
                                }
                            } else {
                                expanded_expr += expr[k];
                            }
                        } else {
                            expanded_expr += expr[k];
                        }
                    }

                    try {
                        out += std::to_string(evaluate_arithmetic_expression(expanded_expr));
                    } catch (const std::runtime_error& e) {
                        throw std::runtime_error(std::string(e.what()) + " while evaluating $((" +
                                                 expr + "))");
                    }
                    i = j + 1;
                    continue;
                }
            }

            if (c == '$' && i + 1 < result.size() && result[i + 1] == '{') {
                size_t brace_depth = 1;
                size_t j = i + 2;
                bool found = false;

                while (j < result.size() && brace_depth > 0) {
                    if (result[j] == '{') {
                        brace_depth++;
                    } else if (result[j] == '}') {
                        brace_depth--;
                        if (brace_depth == 0) {
                            found = true;
                            break;
                        }
                    }
                    j++;
                }

                if (found) {
                    std::string param_expr = result.substr(i + 2, j - (i + 2));
                    std::string expanded_result = expand_parameter_expression(param_expr);

                    if (expanded_result.find('$') != std::string::npos) {
                        size_t dollar_pos = 0;
                        while ((dollar_pos = expanded_result.find('$', dollar_pos)) !=
                               std::string::npos) {
                            size_t var_start = dollar_pos + 1;
                            size_t var_end = var_start;
                            while (var_end < expanded_result.length() &&
                                   (std::isalnum(expanded_result[var_end]) ||
                                    expanded_result[var_end] == '_')) {
                                var_end++;
                            }
                            if (var_end > var_start) {
                                std::string var_name =
                                    expanded_result.substr(var_start, var_end - var_start);
                                std::string var_value = get_variable_value(var_name);
                                (void)expanded_result.replace(dollar_pos, var_end - dollar_pos,
                                                              var_value);
                                dollar_pos += var_value.length();
                            } else {
                                dollar_pos++;
                            }
                        }
                    }

                    out += expanded_result;
                    i = j;
                    continue;
                } else {
                    throw std::runtime_error("syntax error near unexpected token '{'");
                }
            }
        }

        out += c;
    }

    return out;
}
