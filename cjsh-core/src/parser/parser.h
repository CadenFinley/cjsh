/*
  parser.h

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

#ifndef CJSH_CORE_SRC_PARSER_PARSER_H
#define CJSH_CORE_SRC_PARSER_PARSER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Shell;
#include "expansion_engine.h"
#include "tokenizer.h"
#include "variable_expander.h"

enum class CommandRedirectionType : std::uint8_t {
    Input,
    ReadWrite,
    Output,
    Append,
    ForceOutput,
    StderrOutput,
    StderrAppend,
    Duplicate,
    Close,
    BothOutput,
    HereDoc,
    HereString,
    FdInput,
    FdOutput
};

struct CommandRedirection {
    CommandRedirectionType type;
    int fd{-1};
    int target_fd{-1};
    std::string value;
};

struct Command {
    std::vector<std::string> args;
    std::string input_file;
    std::string output_file;
    std::string append_file;
    std::string original_text;
    bool background = false;
    bool auto_background_on_stop = false;
    bool auto_background_on_stop_silent = false;
    bool negate_pipeline = false;
    bool stderr_to_stdout = false;
    bool stdout_to_stderr = false;
    std::string stderr_file;
    bool stderr_append = false;
    std::string here_doc;
    std::string here_string;
    bool both_output = false;
    std::string both_output_file;
    bool force_overwrite = false;

    std::vector<std::pair<int, std::string>> fd_redirections;
    std::vector<std::pair<int, int>> fd_duplications;
    std::vector<CommandRedirection> redirection_order;
    std::vector<std::string> process_substitutions;

    Command();

    void set_fd_redirection(int fd, std::string value);

    void set_fd_duplication(int fd, int target);

    void add_redirection(CommandRedirectionType type, std::string value = "", int fd = -1,
                         int target_fd = -1);

    bool has_fd_redirection(int fd) const;

    bool has_fd_duplication(int fd) const;
};

struct LogicalCommand {
    std::string command;
    std::string op;
};

class Parser {
   public:
    struct HistoryExpansionResult {
        bool has_error = false;
        bool was_expanded = false;
        bool should_echo = false;
        std::string expanded_command;
        std::string error_message;
    };

    HistoryExpansionResult perform_history_expansion(const std::string& command) const;

    std::vector<std::string> parse_into_lines(const std::string& source);
    const std::vector<std::string>& prepare_interactive_input(const std::string& script);
    bool awaiting_here_document() const {
        return incomplete_here_document;
    }

    std::vector<std::string> parse_command(const std::string& cmdline);
    // Reuse lexical words across validation and execution, before live expansion.
    std::vector<std::string> tokenize_command_cached(const std::string& cmdline);
    std::string expand_aliases(const std::string& source) const;
    std::vector<Command> parse_pipeline(const std::string& command);
    std::vector<LogicalCommand> parse_logical_commands(const std::string& command);
    std::vector<std::string> parse_semicolon_commands(const std::string& command,
                                                      bool split_on_newlines = false);
    bool is_env_assignment(const std::string& command, std::string& var_name,
                           std::string& var_value);
    void expand_env_vars(std::string& arg);
    void expand_env_vars_selective(std::string& arg);
    long long evaluate_arithmetic(const std::string& expr);

    std::vector<Command> parse_pipeline_with_preprocessing(const std::string& command);

    void set_aliases(const std::unordered_map<std::string, std::string>& new_aliases);

    void set_env_vars(const std::unordered_map<std::string, std::string>& new_env_vars);
    void set_env_var(const std::string& name, const std::string& value);
    void unset_env_var(const std::string& name);

    void set_shell(Shell* new_shell);

   private:
    void ensure_parsers_initialized();
    std::vector<std::string> prepare_expansion_tokens(std::vector<std::string> args);
    bool is_control_word_at_position(const std::string& command, size_t i, int paren_depth,
                                     int brace_depth, bool in_quotes, int& control_depth);
    void process_heredoc_content(std::string& content);
    bool handle_fd_redirection(const std::string& value, size_t& i,
                               const std::vector<std::string>& tokens, Command& cmd,
                               std::vector<std::string>& filtered_args);

    std::unordered_map<std::string, std::string> aliases;
    std::unordered_map<std::string, std::string> env_vars;
    Shell* shell = nullptr;
    std::map<std::string, std::string> current_here_docs;
    struct PreparedInput {
        std::string source;
        std::vector<std::string> lines;
    };
    std::optional<PreparedInput> prepared_input;
    bool incomplete_here_document = false;
    // Cache only lexical structure, before aliases, variables, IFS, or globbing.
    std::unordered_map<std::string, std::vector<std::string>> command_tokens;
    bool command_tokens_extglob = false;
    int command_tokens_dialect = -1;

    std::unique_ptr<Tokenizer> tokenizer;
    std::unique_ptr<VariableExpander> variableExpander;
    std::unique_ptr<ExpansionEngine> expansionEngine;
};

#endif  // CJSH_CORE_SRC_PARSER_PARSER_H
