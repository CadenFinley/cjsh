/*
  interpreter.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_INTERPRETER_H
#define CJSH_CORE_SRC_INTERPRETER_INTERPRETER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "error_out.h"
#include "function_evaluator.h"
#include "function_ref.h"
#include "parser.h"
#include "pattern_matcher.h"
#include "variable_manager.h"

class ShellScriptInterpreter {
   public:
    static constexpr int exit_return = 253;
    static constexpr int exit_continue = 254;
    static constexpr int exit_break = 255;
    static constexpr int exit_command_not_found = 127;

    ShellScriptInterpreter();
    ~ShellScriptInterpreter();

    ShellScriptInterpreter(const ShellScriptInterpreter&) = delete;
    ShellScriptInterpreter& operator=(const ShellScriptInterpreter&) = delete;
    ShellScriptInterpreter(ShellScriptInterpreter&&) = delete;
    ShellScriptInterpreter& operator=(ShellScriptInterpreter&&) = delete;

    void set_parser(Parser* parser);
    void set_error_source(const std::string& source);
    const std::string& get_error_source() const;

    int execute_block(const std::vector<std::string>& lines, bool skip_validation = false,
                      std::optional<bool> aliases_preexpanded = std::nullopt,
                      function_evaluator::SyntaxValidationCache* syntax_cache = nullptr);
    std::vector<std::string> parse_into_lines(const std::string& script);
    bool needs_additional_input(const std::vector<std::string>& lines);

    enum class ErrorCategory : std::uint8_t {
        SYNTAX,
        CONTROL_FLOW,
        REDIRECTION,
        VARIABLES,
        COMMANDS,
        SEMANTICS,
        STYLE,
        PERFORMANCE
    };

    struct ErrorPosition {
        size_t line_number;
        size_t column_start;
        size_t column_end;
        size_t char_offset;
    };

    struct SyntaxError {
        ErrorPosition position;
        ErrorSeverity severity;
        ErrorCategory category;
        std::string error_code;
        std::string message;
        std::string line_content;
        std::string suggestion;
        std::vector<std::string> related_info;
        std::string documentation_url;

        SyntaxError(size_t line_num, const std::string& msg, const std::string& line_content);
        SyntaxError(ErrorPosition pos, ErrorSeverity sev, ErrorCategory cat,
                    const std::string& code, const std::string& msg,
                    const std::string& line_content = "", const std::string& suggestion = "");
    };

    std::vector<SyntaxError> validate_script_syntax(const std::vector<std::string>& lines);
    bool has_syntax_errors(const std::vector<std::string>& lines, bool print_errors = true);

    std::vector<SyntaxError> validate_comprehensive_syntax(const std::vector<std::string>& lines,
                                                           bool check_semantics = true,
                                                           bool check_style = false);

    std::vector<SyntaxError> validate_variable_usage(const std::vector<std::string>& lines,
                                                     bool include_usage = true);

    std::vector<SyntaxError> validate_command_existence();

    std::vector<SyntaxError> validate_redirection_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_arithmetic_expressions(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_parameter_expansions(const std::vector<std::string>& lines);

    std::vector<SyntaxError> analyze_control_flow();

    std::vector<SyntaxError> check_style_guidelines(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_pipeline_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_function_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_loop_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_conditional_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_array_syntax(const std::vector<std::string>& lines);

    std::vector<SyntaxError> validate_heredoc_syntax(const std::vector<std::string>& lines);

    bool has_function(const std::string& name) const;
    bool unset_function(const std::string& name);
    std::vector<std::string> get_function_names() const;
    int invoke_function(const std::vector<std::string>& args);

    std::string expand_parameter_expression(const std::string& param_expr, bool quoted = false);

    std::string get_variable_value(const std::string& var_name);

    long long evaluate_arithmetic_expression(const std::string& expr);

    void push_function_scope();
    void pop_function_scope();
    void set_local_variable(const std::string& name, const std::string& value);
    bool is_local_variable(const std::string& name) const;
    bool unset_local_variable(const std::string& name);
    void mark_local_as_exported(const std::string& name);
    bool in_function_scope() const;
    void push_source_scope();
    void pop_source_scope();
    bool in_source_scope() const;

    VariableManager& get_variable_manager();

   private:
    struct StructuralSyntax {
        std::vector<std::string> lines;
        bool posix_mode;
        std::vector<SyntaxError> script_errors;
        std::optional<std::vector<SyntaxError>> loop_errors;
        std::optional<std::vector<SyntaxError>> conditional_errors;
    };
    std::optional<StructuralSyntax> structural_syntax;
    StructuralSyntax take_structural_syntax(const std::vector<std::string>& lines);

    Parser* shell_parser = nullptr;
    function_evaluator::FunctionMap functions;

    VariableManager variable_manager;
    PatternMatcher pattern_matcher;

    bool variable_is_set(const std::string& var_name);
    int set_last_status(int code);
    int run_pipeline(const std::vector<Command>& cmds);

    int execute_subshell(const std::string& subshell_content, bool preexpanded = false);
    int execute_function_call(const std::vector<std::string>& expanded_args);
    int handle_env_assignment(const std::vector<std::string>& expanded_args);
    std::optional<int> try_execute_quick_command(const std::string& command, bool* function_call);
    std::optional<int> try_execute_simple_block(const std::vector<std::string>& lines);

    size_t current_line_number = 1;
    std::optional<int> last_substitution_exit_status;
    std::optional<int> pending_assignment_exit_status;

    bool skip_validation_mode = false;
    bool aliases_preexpanded = false;
    std::string error_source;
    size_t source_depth = 0;

    bool should_interpret_as_cjsh_script(const std::string& path) const;

    int evaluate_logical_condition_internal(const std::string& condition,
                                            cjsh::FunctionRef<int(const std::string&)> executor);

    std::string expand_all_substitutions(const std::string& input,
                                         cjsh::FunctionRef<int(const std::string&)> executor);

    struct BlockHandlerResult {
        bool handled;
        int exit_code;
        size_t next_line_index;
    };

    BlockHandlerResult try_dispatch_block_statement(
        const std::vector<std::string>& lines, size_t line_index, const std::string& line,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_if_block,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_for_block,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_select_block,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_while_block,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_until_block,
        cjsh::FunctionRef<int(const std::vector<std::string>&, size_t&)> handle_case_block);

    void push_loop_scope();
    void pop_loop_scope();
    bool in_loop_scope() const;

    int loop_depth = 0;
};

#endif  // CJSH_CORE_SRC_INTERPRETER_INTERPRETER_H
