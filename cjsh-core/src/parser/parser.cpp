/*
  parser.cpp

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

#include "parser.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "builtin.h"
#include "command_preprocessor.h"
#include "delimiter_state.h"
#include "error_out.h"
#include "expansion_engine.h"
#include "flags.h"
#include "history_expansion.h"
#include "interpreter_utils.h"
#include "job_control.h"
#include "parser_utils.h"
#include "quote_info.h"
#include "readonly_command.h"
#include "redirection_utils.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"
#include "tokenizer.h"
#include "variable_expander.h"

Command::Command() {
    args.reserve(8);
    process_substitutions.reserve(2);
    fd_redirections.reserve(2);
    fd_duplications.reserve(2);
    redirection_order.reserve(4);
}

void Command::set_fd_redirection(int fd, std::string value) {
    auto it = std::find_if(fd_redirections.begin(), fd_redirections.end(),
                           [fd](const auto& entry) { return entry.first == fd; });
    if (it != fd_redirections.end()) {
        it->second = std::move(value);
    } else {
        (void)fd_redirections.emplace_back(fd, std::move(value));
    }
}

void Command::set_fd_duplication(int fd, int target) {
    auto it = std::find_if(fd_duplications.begin(), fd_duplications.end(),
                           [fd](const auto& entry) { return entry.first == fd; });
    if (it != fd_duplications.end()) {
        it->second = target;
    } else {
        (void)fd_duplications.emplace_back(fd, target);
    }
}

void Command::add_redirection(CommandRedirectionType type, std::string value, int fd,
                              int target_fd) {
    redirection_order.push_back(CommandRedirection{type, fd, target_fd, std::move(value)});
}

bool Command::has_fd_redirection(int fd) const {
    return std::any_of(fd_redirections.begin(), fd_redirections.end(),
                       [fd](const auto& entry) { return entry.first == fd; });
}

bool Command::has_fd_duplication(int fd) const {
    return std::any_of(fd_duplications.begin(), fd_duplications.end(),
                       [fd](const auto& entry) { return entry.first == fd; });
}

namespace {
bool is_ampersand_inside_arithmetic_or_brackets(const std::string& text, size_t amp_index,
                                                bool respect_quote_escapes) {
    bool in_quotes = false;
    char quote_char = '\0';
    int arith_depth = 0;
    int bracket_depth = 0;
    bool inside = false;

    for (size_t i = 0; i < text.length(); ++i) {
        if ((text[i] == '"' || text[i] == '\'') &&
            (!respect_quote_escapes || !is_char_escaped(text, i))) {
            if (!in_quotes) {
                in_quotes = true;
                quote_char = text[i];
            } else if (quote_char == text[i]) {
                in_quotes = false;
            }
        } else if (!in_quotes) {
            if (i >= 2 && text[i - 2] == '$' && text[i - 1] == '(' && text[i] == '(') {
                arith_depth++;
            } else if (i + 1 < text.length() && text[i] == ')' && text[i + 1] == ')' &&
                       arith_depth > 0) {
                arith_depth--;
                i++;
            } else if (i + 1 < text.length() && text[i] == '[' && text[i + 1] == '[') {
                bracket_depth++;
                i++;
            } else if (i + 1 < text.length() && text[i] == ']' && text[i + 1] == ']' &&
                       bracket_depth > 0) {
                bracket_depth--;
                i++;
            } else if (i == amp_index && text[i] == '&' && (arith_depth > 0 || bracket_depth > 0)) {
                inside = true;
            }
        }
    }

    return inside;
}

using RedirectionToken = redirection_utils::RedirectionOperator;

std::optional<RedirectionToken> parse_redirection_token(std::string_view token) {
    auto parsed = redirection_utils::parse_operator_token(token);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    if (*parsed == RedirectionToken::ReadWrite || *parsed == RedirectionToken::DupInput ||
        *parsed == RedirectionToken::DupOutput) {
        return std::nullopt;
    }

    return parsed;
}

bool redirection_requires_value(RedirectionToken token) {
    return redirection_utils::requires_operand(token);
}

bool is_simple_command_candidate(std::string_view cmdline) {
    bool seen_non_space = false;
    constexpr std::string_view kSpecialChars = "\\\"'|&;<>(){}[]$`#*?=!";

    for (char c : cmdline) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) == 0) {
            seen_non_space = true;
        }
        if (kSpecialChars.find(c) != std::string_view::npos) {
            return false;
        }
    }

    return seen_non_space;
}

bool contains_tilde_character(std::string_view token) {
    return token.find('~') != std::string_view::npos;
}

bool requires_glob_expansion_or_unescape(std::string_view token) {
    if (token.find_first_of("*?[") != std::string_view::npos ||
        token.find('\x1F') != std::string_view::npos) {
        return true;
    }
    if (!config::extglob_enabled) {
        return false;
    }
    for (size_t i = 0; i + 1 < token.size(); ++i) {
        if ((token[i] == '+' || token[i] == '@' || token[i] == '!') && token[i + 1] == '(') {
            return true;
        }
    }
    return false;
}

bool parse_simple_dollar_parameter(std::string_view token, std::string& parameter_name_out) {
    parameter_name_out.clear();

    if (token.size() < 2 || token.front() != '$') {
        return false;
    }

    if (token[1] == '{' || token[1] == '(') {
        return false;
    }

    unsigned char first = static_cast<unsigned char>(token[1]);
    if (std::isdigit(first) != 0) {
        if (token.size() == 2) {
            (void)parameter_name_out.assign(1, token[1]);
            return true;
        }
        return false;
    }

    if (!is_valid_identifier_start(token[1])) {
        return false;
    }

    for (size_t i = 2; i < token.size(); ++i) {
        if (!is_valid_identifier_char(token[i])) {
            return false;
        }
    }

    (void)parameter_name_out.assign(token.substr(1));
    return true;
}

bool is_simple_arithmetic_assignment_candidate(std::string_view command_text) {
    if (command_text.empty()) {
        return false;
    }

    if (command_text.find_first_of(" \t\r\n\"'`|&;<>[]{}#!*?") != std::string_view::npos) {
        return false;
    }

    size_t equals_pos = command_text.find('=');
    if (equals_pos == std::string_view::npos || equals_pos == 0) {
        return false;
    }

    if (!is_valid_identifier_start(command_text.front())) {
        return false;
    }

    for (size_t i = 1; i < equals_pos; ++i) {
        if (!is_valid_identifier_char(command_text[i])) {
            return false;
        }
    }

    std::string_view rhs = command_text.substr(equals_pos + 1);
    if (rhs.size() < 5) {
        return false;
    }

    if (rhs.rfind("$((", 0) != 0 || rhs.substr(rhs.size() - 2) != "))") {
        return false;
    }

    return true;
}

bool contains_internal_substitution_markers(std::string_view text) {
    if (text.find('\x1E') != std::string_view::npos) {
        return true;
    }

    return text.find(subst_literal_start_plain()) != std::string_view::npos ||
           text.find(subst_literal_end_plain()) != std::string_view::npos ||
           text.find(noenv_start_plain()) != std::string_view::npos ||
           text.find(noenv_end_plain()) != std::string_view::npos;
}

std::vector<std::string> fast_split_on_whitespace(std::string_view cmdline) {
    std::vector<std::string> result;
    result.reserve(8);

    size_t i = 0;
    while (i < cmdline.size()) {
        while (i < cmdline.size() && (std::isspace(static_cast<unsigned char>(cmdline[i])) != 0)) {
            ++i;
        }
        if (i >= cmdline.size()) {
            break;
        }
        size_t start = i;
        while (i < cmdline.size() && (std::isspace(static_cast<unsigned char>(cmdline[i])) == 0)) {
            ++i;
        }
        (void)result.emplace_back(cmdline.substr(start, i - start));
    }

    return result;
}

bool handle_arithmetic_expansion(std::string_view text, size_t idx, size_t pos, bool in_double,
                                 int& arith_depth) {
    if (idx + 2 >= pos || idx + 2 >= text.size()) {
        return false;
    }

    char current = text[idx];

    if (current == '$' && text[idx + 1] == '(' && text[idx + 2] == '(') {
        arith_depth++;
        return true;
    }

    if (!in_double && current == '(' && text[idx + 1] == '(' && idx + 1 < pos) {
        arith_depth++;
        return true;
    }

    if (arith_depth > 0 && current == ')' && text[idx + 1] == ')' && idx + 1 < pos) {
        arith_depth--;
        return true;
    }

    return false;
}

bool handle_double_bracket_open(const std::string& command, size_t i, std::string& current,
                                DelimiterState& delimiters, size_t& index_increment) {
    if (delimiters.in_quotes || i + 1 >= command.length() || command[i + 1] != '[') {
        return false;
    }
    delimiters.bracket_depth++;
    current += command[i];
    current += command[i + 1];
    index_increment = 1;
    return true;
}

bool handle_double_bracket_close(const std::string& command, size_t i, std::string& current,
                                 DelimiterState& delimiters, size_t& index_increment) {
    if (delimiters.in_quotes || delimiters.bracket_depth <= 0 || i + 1 >= command.length() ||
        command[i + 1] != ']') {
        return false;
    }
    delimiters.bracket_depth--;
    current += command[i];
    current += command[i + 1];
    index_increment = 1;
    return true;
}

bool line_has_continuation(const std::string& line, bool ended_in_comment) {
    if (ended_in_comment) {
        return false;
    }

    return has_line_continuation_suffix(line);
}

std::vector<std::string> merge_line_continuations(const std::vector<std::string>& lines,
                                                  const std::vector<bool>& comment_flags) {
    if (lines.empty()) {
        return lines;
    }

    std::vector<std::string> merged;
    merged.reserve(lines.size());
    std::string accumulator;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (accumulator.empty()) {
            accumulator = line;
        } else {
            accumulator += line;
        }

        if (line_has_continuation(line, comment_flags[i])) {
            while (!accumulator.empty() &&
                   (accumulator.back() == ' ' || accumulator.back() == '\t')) {
                accumulator.pop_back();
            }
            if (!accumulator.empty()) {
                accumulator.pop_back();
            }
            continue;
        }

        merged.push_back(accumulator);
        accumulator.clear();
    }

    if (!accumulator.empty()) {
        merged.push_back(accumulator);
    }

    return merged;
}

}  // namespace

Parser::HistoryExpansionResult Parser::perform_history_expansion(const std::string& command) const {
    HistoryExpansionResult result;
    result.expanded_command = command;

    if (command.empty()) {
        return result;
    }

    if (!config::history_expansion_enabled) {
        return result;
    }

    if (isatty(STDIN_FILENO) == 0) {
        return result;
    }

    auto history_entries = HistoryExpansion::read_history_entries();
    // The interactive loop records commands after execution; the current input is not staged.
    auto expansion = HistoryExpansion::expand(command, history_entries);

    if (expansion.has_error) {
        result.has_error = true;
        result.error_message = expansion.error_message;
        return result;
    }

    if (expansion.was_expanded) {
        result.was_expanded = true;
        result.expanded_command = expansion.expanded_command;
    }

    result.should_echo = expansion.should_echo;
    return result;
}

void Parser::process_heredoc_content(std::string& content) {
    if (content.length() >= 10 && content.substr(0, 10) == "__EXPAND__") {
        content = content.substr(10);
        if (!variableExpander) {
            variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
        }
        variableExpander->expand_env_vars(content);
        variableExpander->expand_command_substitutions_in_string(content);
    }
    (void)strip_subst_literal_markers(content);
}

bool Parser::is_control_word_at_position(const std::string& command, size_t i, int paren_depth,
                                         int brace_depth, bool in_quotes, int& control_depth) {
    if (in_quotes || paren_depth != 0 || brace_depth != 0) {
        return false;
    }

    if (command[i] != ' ' && command[i] != '\t' && i != 0) {
        return false;
    }

    size_t word_start = (command[i] == ' ' || command[i] == '\t') ? i + 1 : i;
    std::string word;
    size_t j = word_start;
    while (j < command.length() && std::isalpha(command[j])) {
        word += command[j];
        j++;
    }

    auto control_word = parse_parser_control_token(word);
    if (!control_word.has_value()) {
        return false;
    }

    if (parser_control_token_is_opening(*control_word)) {
        control_depth++;
        return true;
    }

    if (parser_control_token_is_closing(*control_word) && control_depth > 0) {
        control_depth--;
        return true;
    }

    return false;
}

bool Parser::handle_fd_redirection(const std::string& value, size_t& i,
                                   const std::vector<std::string>& tokens, Command& cmd,
                                   std::vector<std::string>& filtered_args) {
    if (value.length() <= 1 || i + 1 >= tokens.size()) {
        return false;
    }

    char op = value.back();
    if ((op != '<' && op != '>') || !std::isdigit(value[0])) {
        return false;
    }

    try {
        int fd = std::stoi(value.substr(0, value.length() - 1));
        std::string file = QuoteInfo(tokens[++i]).value;
        std::string direction = (op == '<') ? "input:" : "output:";
        cmd.set_fd_redirection(fd, direction + file);
        cmd.add_redirection(
            (op == '<') ? CommandRedirectionType::FdInput : CommandRedirectionType::FdOutput, file,
            fd);
        return true;
    } catch (const std::exception&) {
        filtered_args.push_back(tokens[i]);
        return false;
    }
}

void Parser::ensure_parsers_initialized() {
    if (!tokenizer) {
        tokenizer = std::make_unique<Tokenizer>();
    }
    if (!variableExpander) {
        variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
    }
    if (!expansionEngine) {
        expansionEngine = std::make_unique<ExpansionEngine>(shell);
    }
}

void Parser::set_aliases(const std::unordered_map<std::string, std::string>& new_aliases) {
    aliases = new_aliases;
}

void Parser::set_env_vars(const std::unordered_map<std::string, std::string>& new_env_vars) {
    env_vars = new_env_vars;
}

void Parser::set_env_var(const std::string& name, const std::string& value) {
    env_vars[name] = value;
}

void Parser::unset_env_var(const std::string& name) {
    (void)env_vars.erase(name);
}

void Parser::set_shell(Shell* new_shell) {
    shell = new_shell;
    ensure_parsers_initialized();
}

const std::vector<std::string>& Parser::prepare_interactive_input(const std::string& script) {
    auto lines = parse_into_lines(script);
    prepared_input = PreparedInput{script, std::move(lines)};
    return prepared_input->lines;
}

std::vector<std::string> Parser::parse_into_lines(const std::string& script) {
    if (prepared_input) {
        auto prepared = std::move(*prepared_input);
        prepared_input.reset();
        if (prepared.source == script) {
            return std::move(prepared.lines);
        }
    }
    // shared script splitter used by shell::execute and interactive continuation checks
    // control-flow blocks like if/then/fi depend on this producing stable logical line chunks
    std::vector<std::string> lines;

    lines.reserve(std::min(script.length() / 30 + 2, size_t(64)));
    std::vector<bool> line_comment_flags;
    line_comment_flags.reserve(lines.capacity());
    size_t start = 0;
    bool in_quotes = false;
    char quote_char = '\0';
    bool in_here_doc = false;
    bool strip_tabs = false;
    bool here_doc_expand = true;
    size_t here_doc_operator_pos = std::string::npos;
    size_t here_doc_operator_len = 0;
    size_t here_doc_delim_end_pos = std::string::npos;
    std::string here_doc_delimiter;
    here_doc_delimiter.reserve(32);
    std::string here_doc_content;

    here_doc_content.reserve(512);
    std::string current_here_doc_line;
    current_here_doc_line.reserve(128);

    bool line_is_comment = false;
    bool in_parameter_brace = false;

    auto add_here_doc_placeholder_line = [&](std::string before, const std::string& rest) {
        std::string placeholder;
        placeholder.reserve(32);
        placeholder = "HEREDOC_PLACEHOLDER_";
        placeholder += std::to_string(lines.size());

        std::string stored_content;
        if (here_doc_expand) {
            stored_content.reserve(here_doc_content.size() + 10);
            stored_content = "__EXPAND__";
            stored_content += here_doc_content;
        } else {
            stored_content = std::move(here_doc_content);
        }
        current_here_docs[placeholder] = std::move(stored_content);

        std::string segment = std::move(before);
        segment.reserve(segment.size() + placeholder.size() + rest.size() + 2);
        segment += "< ";
        segment += placeholder;
        segment += rest;

        if (!segment.empty() && segment.back() == '\r') {
            segment.pop_back();
        }
        lines.push_back(std::move(segment));
        line_comment_flags.push_back(false);
    };

    auto is_in_arithmetic_context = [](std::string_view text, size_t pos) -> bool {
        bool in_single = false;
        bool in_double = false;
        bool escaped = false;
        int arith_depth = 0;

        size_t idx = 0;
        while (idx < pos && idx < text.size()) {
            char ch = text[idx];

            if (escaped) {
                escaped = false;
                idx++;
                continue;
            }

            if (ch == '\\' && !in_single) {
                escaped = true;
                idx++;
                continue;
            }

            if (in_single) {
                if (ch == '\'') {
                    in_single = false;
                }
                idx++;
                continue;
            }

            if (ch == '\'') {
                in_single = true;
                idx++;
                continue;
            }

            if (in_double) {
                if (ch == '"') {
                    in_double = false;
                    idx++;
                    continue;
                }
                if (::handle_arithmetic_expansion(text, idx, pos, in_double, arith_depth)) {
                    if (ch == '$') {
                        idx += 3;
                    } else {
                        idx += 2;
                    }
                    continue;
                }
                idx++;
                continue;
            }

            if (ch == '"') {
                in_double = true;
                idx++;
                continue;
            }

            if (::handle_arithmetic_expansion(text, idx, pos, in_double, arith_depth)) {
                if (ch == '$') {
                    idx += 3;
                } else if (ch == '(') {
                    idx += 2;
                } else {
                    idx += 2;
                }
                continue;
            }

            idx++;
        }

        return arith_depth > 0;
    };

    auto locate_here_operator = [&](std::string_view sv, size_t& pos_out,
                                    size_t& delim_end_out) -> bool {
        size_t pos = sv.find("<<");
        while (pos != std::string::npos) {
            HereDocHeader header;
            if (!parse_here_doc_header(sv, pos, header)) {
                pos = sv.find("<<", pos + 2);
                continue;
            }
            size_t op_len = header.operator_length;

            if (is_in_arithmetic_context(sv, pos)) {
                pos = sv.find("<<", pos + op_len);
                continue;
            }

            if (header.delimiter == here_doc_delimiter) {
                pos_out = pos;
                delim_end_out = header.delimiter_end;
                return true;
            }

            pos = sv.find("<<", pos + op_len);
        }
        return false;
    };

    auto process_here_doc_segment = [&](std::string_view segment_view) {
        bool segment_created = false;

        auto append_placeholder_from_offsets = [&](size_t delim_end_in_segment) {
            if (here_doc_operator_pos > segment_view.size()) {
                return;
            }
            size_t effective_delim_end = std::min(delim_end_in_segment, segment_view.size());

            std::string before_here{segment_view.substr(0, here_doc_operator_pos)};
            size_t line_end = segment_view.find('\n', effective_delim_end);
            std::string rest_of_line;
            if (line_end != std::string::npos) {
                (void)rest_of_line.assign(
                    segment_view.substr(effective_delim_end, line_end - effective_delim_end));
            } else {
                (void)rest_of_line.assign(segment_view.substr(effective_delim_end));
            }

            add_here_doc_placeholder_line(std::move(before_here), rest_of_line);
            segment_created = true;
        };

        if (!segment_created && here_doc_operator_pos != std::string::npos &&
            here_doc_operator_len > 0) {
            append_placeholder_from_offsets(here_doc_delim_end_pos);
        }

        if (!segment_created) {
            size_t match_pos = std::string::npos;
            size_t delim_end = std::string::npos;
            if (locate_here_operator(segment_view, match_pos, delim_end)) {
                std::string before_here{segment_view.substr(0, match_pos)};
                size_t line_end = segment_view.find('\n', delim_end);
                std::string rest_of_line;
                if (line_end != std::string::npos) {
                    (void)rest_of_line.assign(segment_view.substr(delim_end, line_end - delim_end));
                } else {
                    (void)rest_of_line.assign(segment_view.substr(delim_end));
                }

                add_here_doc_placeholder_line(std::move(before_here), rest_of_line);
                segment_created = true;
            }
        }

        if (!segment_created) {
            std::string segment{segment_view};
            if (!segment.empty() && segment.back() == '\r') {
                segment.pop_back();
            }
            lines.push_back(std::move(segment));
            line_comment_flags.push_back(false);
        }
    };

    auto reset_here_doc_state = [&] {
        here_doc_operator_pos = std::string::npos;
        here_doc_operator_len = 0;
        here_doc_delim_end_pos = std::string::npos;
        in_here_doc = false;
        strip_tabs = false;
        here_doc_expand = true;
        here_doc_delimiter.clear();
        here_doc_content.clear();
        current_here_doc_line.clear();
    };

    for (size_t i = 0; i < script.size(); ++i) {
        char c = script[i];

        if (in_here_doc) {
            if (c == '\n') {
                std::string trimmed_line = ::trim_here_doc_compare_line(current_here_doc_line);

                if (trimmed_line == here_doc_delimiter) {
                    std::string_view segment_view{script.data() + start, i - start};
                    process_here_doc_segment(segment_view);
                    reset_here_doc_state();
                    start = i + 1;
                } else {
                    if (!here_doc_content.empty()) {
                        here_doc_content += '\n';
                    }

                    std::string line_to_add = current_here_doc_line;
                    if (strip_tabs) {
                        size_t first_non_tab = line_to_add.find_first_not_of('\t');
                        if (first_non_tab != std::string::npos) {
                            line_to_add = line_to_add.substr(first_non_tab);
                        } else {
                            line_to_add.clear();
                        }
                    }
                    here_doc_content += line_to_add;
                }
                current_here_doc_line.clear();
            } else {
                current_here_doc_line += c;
            }
            continue;
        }

        if (!line_is_comment && !in_quotes) {
            if (!in_parameter_brace && c == '$' && i + 1 < script.size() && script[i + 1] == '{') {
                in_parameter_brace = true;
            } else if (in_parameter_brace && c == '}') {
                in_parameter_brace = false;
            }
        }

        if (!line_is_comment && !in_quotes && !in_parameter_brace && c == '#' &&
            !is_char_escaped(script, i)) {
            size_t line_end = script.find('\n', start);
            if (line_end == std::string::npos) {
                line_end = script.size();
            }
            size_t comment_start = shell_script_interpreter::detail::find_inline_comment_start(
                script, start, line_end);
            if (comment_start == i) {
                line_is_comment = true;
            }
        }

        if (!line_is_comment) {
            if (!in_quotes && (c == '"' || c == '\'') && !is_char_escaped(script, i)) {
                in_quotes = true;
                quote_char = c;
            } else if (in_quotes && c == quote_char) {
                bool escaped_double_quote = (quote_char == '"') && is_char_escaped(script, i);
                if (!escaped_double_quote) {
                    in_quotes = false;
                }
            }
        }

        if (line_is_comment && c == '\n') {
            line_is_comment = false;
        }

        if (!in_quotes && c == '\n') {
            std::string_view segment_view{script.data() + start, i - start};
            if (!in_quotes && segment_view.find("<<") != std::string_view::npos) {
                std::string segment_no_comment =
                    shell_script_interpreter::detail::strip_inline_comment(
                        std::string(segment_view));

                size_t search_from = 0;
                size_t here_pos = std::string::npos;

                while (search_from < segment_no_comment.size()) {
                    size_t candidate = segment_no_comment.find("<<", search_from);
                    if (candidate == std::string::npos) {
                        break;
                    }

                    HereDocHeader header;
                    if (!parse_here_doc_header(segment_no_comment, candidate, header)) {
                        search_from = candidate + 2;
                        continue;
                    }
                    size_t op_len = header.operator_length;

                    if (is_in_arithmetic_context(segment_no_comment, candidate)) {
                        search_from = candidate + op_len;
                        continue;
                    }

                    here_pos = candidate;
                    break;
                }

                if (here_pos == std::string::npos) {
                    goto normal_line_processing;
                }

                HereDocHeader parsed_header;
                if (!parse_here_doc_header(segment_no_comment, here_pos, parsed_header)) {
                    goto normal_line_processing;
                }

                here_doc_operator_pos = here_pos;
                here_doc_operator_len = parsed_header.operator_length;
                here_doc_delim_end_pos = parsed_header.delimiter_end;
                here_doc_delimiter = parsed_header.delimiter;
                here_doc_expand = parsed_header.expand;
                in_here_doc = true;
                strip_tabs = parsed_header.strip_tabs;
                here_doc_content.clear();
                current_here_doc_line.clear();
                continue;
            }

        normal_line_processing:

            std::string segment{script.data() + start, i - start};
            if (!segment.empty() && segment.back() == '\r') {
                segment.pop_back();
            }
            bool ended_in_comment = line_is_comment;
            lines.push_back(std::move(segment));
            line_comment_flags.push_back(ended_in_comment);
            start = i + 1;
            line_is_comment = false;
        }
    }

    if (in_here_doc) {
        std::string trimmed_line = ::trim_here_doc_compare_line(current_here_doc_line);

        if (trimmed_line == here_doc_delimiter) {
            size_t segment_len = script.size() - start;
            if (segment_len >= current_here_doc_line.size()) {
                segment_len -= current_here_doc_line.size();
            } else {
                segment_len = 0;
            }

            if (segment_len > 0 && script[start + segment_len - 1] == '\n') {
                segment_len--;
            }
            if (segment_len > 0 && script[start + segment_len - 1] == '\r') {
                segment_len--;
            }

            std::string_view segment_view{script.data() + start, segment_len};
            process_here_doc_segment(segment_view);
            reset_here_doc_state();
            start = script.size();
        } else {
            std::string tail{script.substr(start)};
            if (!tail.empty() && tail.back() == '\r') {
                tail.pop_back();
            }
            lines.push_back(std::move(tail));
            line_comment_flags.push_back(false);
            reset_here_doc_state();
        }
    }

    if (start <= script.size()) {
        std::string_view tail_view{script.data() + start, script.size() - start};
        if (!tail_view.empty()) {
            std::string tail{tail_view};
            if (!tail.empty() && tail.back() == '\r') {
                tail.pop_back();
            }
            lines.push_back(std::move(tail));
            line_comment_flags.push_back(false);
        }
    }

    if (lines.size() == line_comment_flags.size()) {
        // preserve multiline continuation semantics so partial if headers can span input lines
        return merge_command_group_lines(merge_line_continuations(lines, line_comment_flags));
    }

    return merge_command_group_lines(lines);
}

std::vector<std::string> Parser::prepare_expansion_tokens(std::vector<std::string> args) {
    ensure_parsers_initialized();

    std::vector<std::string> expanded_args;
    expanded_args.reserve(args.empty() ? 8 : args.size() * 3);
    for (std::string& raw_arg : args) {
        QuoteInfo qi(raw_arg);

        if (qi.is_unquoted() && !looks_like_assignment(qi.value) &&
            !contains_internal_substitution_markers(raw_arg) &&
            raw_arg.find('{') != std::string::npos && raw_arg.find('}') != std::string::npos) {
            auto brace_expansions = expansionEngine->expand_braces(raw_arg);
            (void)expanded_args.insert(expanded_args.end(),
                                       std::make_move_iterator(brace_expansions.begin()),
                                       std::make_move_iterator(brace_expansions.end()));
        } else {
            (void)expanded_args.emplace_back(std::move(raw_arg));
        }
    }
    args = std::move(expanded_args);

    auto find_expandable_dollar_ats = [](const std::string& value) {
        const std::string& start_marker = noenv_start();
        const std::string& end_marker = noenv_end();
        std::vector<size_t> positions;
        bool inside_noenv = false;

        for (size_t i = 0; i + 1 < value.size();) {
            if (value.compare(i, start_marker.size(), start_marker) == 0) {
                inside_noenv = true;
                i += start_marker.size();
                continue;
            }
            if (value.compare(i, end_marker.size(), end_marker) == 0) {
                inside_noenv = false;
                i += end_marker.size();
                continue;
            }
            if (!inside_noenv && value[i] == '$' && value[i + 1] == '@') {
                positions.push_back(i);
                i += 2;
                continue;
            }
            ++i;
        }

        return positions;
    };

    std::vector<std::string> pre_expanded_args;
    pre_expanded_args.reserve(args.size() + 4);
    for (std::string& raw_arg : args) {
        QuoteInfo qi(raw_arg);

        const std::vector<size_t> at_positions =
            qi.is_double ? find_expandable_dollar_ats(qi.value) : std::vector<size_t>{};
        if (!at_positions.empty()) {
            auto params = flags::get_positional_parameters();
            std::vector<std::string> fields(1);
            size_t cursor = 0;

            for (size_t at_pos : at_positions) {
                fields.back() += qi.value.substr(cursor, at_pos - cursor);
                if (!params.empty()) {
                    fields.back() += params.front();
                    for (size_t param_index = 1; param_index < params.size(); ++param_index) {
                        fields.push_back(params[param_index]);
                    }
                }
                cursor = at_pos + 2;
            }
            fields.back() += qi.value.substr(cursor);

            if (!params.empty() || fields.size() > 1 || !fields.front().empty()) {
                for (auto& field : fields) {
                    pre_expanded_args.push_back(create_quote_tag(QUOTE_DOUBLE, field));
                }
            }
            continue;
        }
        (void)pre_expanded_args.emplace_back(std::move(raw_arg));
    }
    args = std::move(pre_expanded_args);

    return args;
}

std::vector<std::string> Parser::parse_command(const std::string& cmdline) {
    ensure_parsers_initialized();

    std::vector<std::string> args;
    const bool simple_candidate = is_simple_command_candidate(cmdline);
    const bool arithmetic_assignment_candidate =
        !simple_candidate && is_simple_arithmetic_assignment_candidate(cmdline);

    if (simple_candidate) {
        args = fast_split_on_whitespace(cmdline);
    } else if (arithmetic_assignment_candidate) {
        (void)args.emplace_back(cmdline);
    } else {
        args.reserve(16);
        try {
            args = Tokenizer::tokenize_command(cmdline);
        } catch (const std::exception&) {
            return args;
        }
    }

    if (simple_candidate && !args.empty()) {
        auto alias_it = aliases.find(args[0]);
        if (alias_it == aliases.end()) {
            bool has_tilde = std::any_of(args.begin(), args.end(), [](const std::string& token) {
                return contains_tilde_character(token);
            });

            bool needs_custom_ifs_split = false;
            auto ifs_it = env_vars.find("IFS");
            if (ifs_it != env_vars.end()) {
                const std::string& ifs_value = ifs_it->second;
                for (char delimiter : ifs_value) {
                    if (delimiter == ' ' || delimiter == '\t' || delimiter == '\n') {
                        continue;
                    }
                    needs_custom_ifs_split = std::any_of(
                        args.begin(), args.end(), [delimiter](const std::string& token) {
                            return token.find(delimiter) != std::string::npos;
                        });
                    if (needs_custom_ifs_split) {
                        break;
                    }
                }
            }

            if (!has_tilde && !contains_internal_substitution_markers(cmdline) &&
                !needs_custom_ifs_split) {
                return args;
            }
        }
    }

    if (!args.empty()) {
        auto alias_it = aliases.find(args[0]);
        if (alias_it != aliases.end()) {
            std::vector<std::string> alias_args;
            alias_args.reserve(8);

            try {
                alias_args = Tokenizer::tokenize_command(alias_it->second);

                if (!alias_args.empty()) {
                    std::vector<std::string> new_args;
                    new_args.reserve(alias_args.size() + args.size());
                    (void)new_args.insert(new_args.end(), alias_args.begin(), alias_args.end());
                    if (args.size() > 1) {
                        (void)new_args.insert(new_args.end(), args.begin() + 1, args.end());
                    }

                    args = new_args;

                    bool has_pipe = false;
                    for (const auto& arg : args) {
                        if (arg == "|") {
                            has_pipe = true;
                            break;
                        }
                    }

                    if (has_pipe) {
                        return {"__ALIAS_PIPELINE__", alias_it->second};
                    }
                }
            } catch (const std::exception&) {
                // Alias expansion is optional; keep original args on failure.
            }
        }
    }

    args = prepare_expansion_tokens(std::move(args));

    auto report_environment_expansion_error = [&](const std::runtime_error& error) {
        const std::string message = error.what();
        if (shell != nullptr && shell->get_shell_option(ShellOption::Nounset) &&
            message.find("parameter not set") != std::string::npos) {
            print_error({ErrorType::RUNTIME_ERROR,
                         ErrorSeverity::ERROR,
                         "parser",
                         message,
                         {"Disable 'set -u' or ensure all parameters are defined before "
                          "expansion."}});
            return true;
        }
        print_error({ErrorType::RUNTIME_ERROR,
                     ErrorSeverity::WARNING,
                     "parser",
                     "Error expanding environment variables: " + message,
                     {"Check that referenced variables are set or properly quoted."}});
        return false;
    };

    auto expand_env_value = [&](const std::string& value) -> std::string {
        auto [noenv_stripped, had_noenv] = strip_noenv_sentinels(value);
        if (!had_noenv) {
            std::string parameter_name;
            if (parse_simple_dollar_parameter(noenv_stripped, parameter_name)) {
                try {
                    noenv_stripped = variableExpander->resolve_parameter_value(parameter_name);
                } catch (const std::runtime_error& e) {
                    if (report_environment_expansion_error(e)) {
                        throw;
                    }
                }
                (void)strip_subst_literal_markers(noenv_stripped);
                return noenv_stripped;
            }

            try {
                if (variableExpander->get_use_exported_vars_only()) {
                    variableExpander->expand_exported_env_vars_only(noenv_stripped);
                } else {
                    variableExpander->expand_env_vars(noenv_stripped);
                }
            } catch (const std::runtime_error& e) {
                if (report_environment_expansion_error(e)) {
                    throw;
                }
            }
            (void)strip_subst_literal_markers(noenv_stripped);
            return noenv_stripped;
        }

        std::string value_to_expand = value;
        try {
            variableExpander->expand_env_vars_selective(value_to_expand);
        } catch (const std::runtime_error& e) {
            print_error(
                {ErrorType::RUNTIME_ERROR,
                 ErrorSeverity::WARNING,
                 "parser",
                 std::string("Error in selective environment variable expansion: ") + e.what(),
                 {"Check variable names and ensure they are exported when required."}});
        }
        (void)strip_subst_literal_markers(value_to_expand);
        auto stripped_pair = strip_noenv_sentinels(value_to_expand);
        return std::move(stripped_pair.first);
    };

    std::string command_name;
    if (!args.empty()) {
        command_name = QuoteInfo(args[0]).value;
    }

    const bool is_assignment_builtin = command_name == "export" || command_name == "declare" ||
                                       command_name == "typeset" || command_name == "local" ||
                                       command_name == "readonly";

    for (size_t arg_index = 0; arg_index < args.size(); ++arg_index) {
        std::string& raw_arg = args[arg_index];
        QuoteInfo qi(raw_arg);

        if (qi.is_single) {
            continue;
        }

        if (is_assignment_builtin && arg_index > 0) {
            std::string value = qi.value;
            size_t eq_pos = value.find('=');
            if (eq_pos == std::string::npos) {
                raw_arg = qi.is_double ? create_quote_tag(QUOTE_DOUBLE, value) : value;
                continue;
            }

            std::string name_part = value.substr(0, eq_pos);
            std::string value_part = value.substr(eq_pos + 1);
            std::string expanded_value = expand_env_value(value_part);
            std::string recombined = name_part;
            recombined.append("=").append(expanded_value);
            raw_arg = qi.is_double ? create_quote_tag(QUOTE_DOUBLE, recombined) : recombined;
            continue;
        }
        std::string expanded_value = expand_env_value(qi.value);
        raw_arg = qi.is_double ? create_quote_tag(QUOTE_DOUBLE, expanded_value) : expanded_value;
    }

    std::vector<std::string> ifs_expanded_args;
    ifs_expanded_args.reserve(args.size() * 2);
    for (std::string& raw_arg : args) {
        QuoteInfo qi(raw_arg);

        if (qi.is_unquoted()) {
            std::vector<std::string> split_words = tokenizer->split_by_ifs(raw_arg);
            (void)ifs_expanded_args.insert(ifs_expanded_args.end(),
                                           std::make_move_iterator(split_words.begin()),
                                           std::make_move_iterator(split_words.end()));
        } else {
            (void)ifs_expanded_args.emplace_back(std::move(raw_arg));
        }
    }
    args = std::move(ifs_expanded_args);
    auto tilde_expanded_args = expand_tilde_tokens(args);

    std::vector<std::string> final_args;
    final_args.reserve(tilde_expanded_args.size() * 2);

    bool is_double_bracket_command =
        !tilde_expanded_args.empty() && QuoteInfo(tilde_expanded_args[0]).value == "[[";

    for (const auto& raw_arg : tilde_expanded_args) {
        QuoteInfo qi(raw_arg);

        if (qi.is_unquoted() && !is_double_bracket_command && !looks_like_assignment(qi.value) &&
            requires_glob_expansion_or_unescape(qi.value)) {
            auto gw = expansionEngine->expand_wildcards(qi.value);
            (void)final_args.insert(final_args.end(), gw.begin(), gw.end());
        } else {
            final_args.push_back(qi.value);
        }
    }
    return final_args;
}

std::vector<Command> Parser::parse_pipeline(const std::string& command) {
    std::vector<Command> commands;
    commands.reserve(4);
    std::vector<std::string> command_parts;
    command_parts.reserve(4);

    std::string current;
    current.reserve(command.length() / 4);
    DelimiterState delimiters;

    for (size_t i = 0; i < command.length(); ++i) {
        if (delimiters.update_quote(command[i])) {
            current += command[i];
        } else if (!delimiters.in_quotes && command[i] == '(') {
            delimiters.paren_depth++;
            current += command[i];
        } else if (!delimiters.in_quotes && command[i] == ')') {
            delimiters.paren_depth--;
            current += command[i];
        } else if (command[i] == '[') {
            size_t increment = 0;
            if (handle_double_bracket_open(command, i, current, delimiters, increment)) {
                i += increment;
            } else {
                current += command[i];
            }
        } else if (command[i] == ']') {
            size_t increment = 0;
            if (handle_double_bracket_close(command, i, current, delimiters, increment)) {
                i += increment;
            } else {
                current += command[i];
            }
        } else if (command[i] == '|' && !delimiters.in_quotes && delimiters.paren_depth == 0 &&
                   delimiters.bracket_depth == 0) {
            if (i > 0 && command[i - 1] == '>') {
                current += command[i];
            } else {
                if (!current.empty()) {
                    command_parts.push_back(std::move(current));
                    current.clear();
                    current.reserve(command.length() / 4);
                }
            }
        } else {
            current += command[i];
        }
    }

    if (!current.empty()) {
        command_parts.push_back(std::move(current));
    }

    bool pipeline_negated = false;

    for (size_t cmd_idx = 0; cmd_idx < command_parts.size(); ++cmd_idx) {
        const auto& cmd_str = command_parts[cmd_idx];
        Command cmd;
        std::string cmd_part = cmd_str;

        bool is_background = false;
        bool auto_background_on_stop = false;
        bool auto_background_on_stop_silent = false;
        std::string trimmed = trim_trailing_whitespace(cmd_part);

        if (cmd_idx == 0) {
            std::string leading_trimmed = trim_leading_whitespace(trimmed);
            if (!leading_trimmed.empty() && leading_trimmed.front() == '!') {
                size_t after_bang = 1;
                bool has_whitespace_after_bang =
                    (leading_trimmed.size() == 1) ||
                    (std::isspace(static_cast<unsigned char>(leading_trimmed[after_bang])) != 0);

                if (has_whitespace_after_bang) {
                    pipeline_negated = true;
                    (void)leading_trimmed.erase(0, 1);
                    cmd_part = trim_leading_whitespace(leading_trimmed);
                    trimmed = trim_trailing_whitespace(cmd_part);

                    if (cmd_part.empty()) {
                        throw std::runtime_error(
                            "cjsh: syntax error near unexpected token `newline'");
                    }
                }
            }
        }

        if (trimmed.size() >= 3 && trimmed.substr(trimmed.size() - 3) == "&^!") {
            size_t amp_index = trimmed.size() - 3;

            if (!is_char_escaped(trimmed, amp_index)) {
                bool inside_arithmetic =
                    is_ampersand_inside_arithmetic_or_brackets(trimmed, amp_index, true);

                if (!inside_arithmetic) {
                    auto_background_on_stop = true;
                    auto_background_on_stop_silent = true;
                    cmd_part = trim_trailing_whitespace(trimmed.substr(0, trimmed.size() - 3));
                    trimmed = trim_trailing_whitespace(cmd_part);
                }
            }
        } else if (trimmed.size() >= 2 && trimmed.substr(trimmed.size() - 2) == "&^") {
            size_t amp_index = trimmed.size() - 2;

            if (!is_char_escaped(trimmed, amp_index)) {
                bool inside_arithmetic =
                    is_ampersand_inside_arithmetic_or_brackets(trimmed, amp_index, true);

                if (!inside_arithmetic) {
                    auto_background_on_stop = true;
                    cmd_part = trim_trailing_whitespace(trimmed.substr(0, trimmed.size() - 2));
                    trimmed = trim_trailing_whitespace(cmd_part);
                }
            }
        }

        if (!trimmed.empty() && trimmed.back() == '&' &&
            !is_char_escaped(trimmed, trimmed.size() - 1)) {
            size_t amp_index = trimmed.size() - 1;
            bool inside_arithmetic =
                is_ampersand_inside_arithmetic_or_brackets(trimmed, amp_index, false);

            if (!inside_arithmetic) {
                size_t amp_pos = cmd_part.rfind('&');
                if (amp_pos != std::string::npos && amp_pos + 1 < cmd_part.length()) {
                    size_t next_non_ws = amp_pos + 1;
                    while (next_non_ws < cmd_part.length() &&
                           (std::isspace(cmd_part[next_non_ws]) != 0)) {
                        next_non_ws++;
                    }

                    is_background =
                        next_non_ws >= cmd_part.length() || cmd_part[next_non_ws] != '>';
                } else {
                    is_background = true;
                }
            }
        }

        if (is_background) {
            cmd.background = true;
            cmd_part = trim_trailing_whitespace(trimmed.substr(0, trimmed.length() - 1));
        }

        if (auto_background_on_stop) {
            cmd.auto_background_on_stop = true;
        }
        if (auto_background_on_stop_silent) {
            cmd.auto_background_on_stop_silent = true;
        }

        cmd.original_text = trim_trailing_whitespace(trim_leading_whitespace(cmd_part));

        if (!cmd_part.empty()) {
            size_t lead = cmd_part.find_first_not_of(" \t\r\n");
            if (lead != std::string::npos && (cmd_part[lead] == '(' || cmd_part[lead] == '{')) {
                const bool is_brace_group = cmd_part[lead] == '{';
                size_t close_pos = is_brace_group ? find_matching_brace(cmd_part, lead)
                                                  : find_matching_paren(cmd_part, lead);
                if (close_pos != std::string::npos) {
                    std::string group_content = cmd_part.substr(lead + 1, close_pos - (lead + 1));
                    std::string remaining = cmd_part.substr(close_pos + 1);

                    if (is_brace_group) {
                        group_content = string_utils::trim_ascii_whitespace_copy(group_content);
                        if (!group_content.empty() && group_content.back() == ';') {
                            group_content.pop_back();
                            group_content =
                                string_utils::trim_right_ascii_whitespace_copy(group_content);
                        }
                    }

                    cmd.args.push_back(is_brace_group ? "__INTERNAL_BRACE_GROUP__"
                                                      : "__INTERNAL_SUBSHELL__");
                    cmd.args.push_back(group_content);

                    if (!remaining.empty()) {
                        std::vector<std::string> merged_redir =
                            Tokenizer::tokenize_command(remaining);

                        for (size_t i = 0; i < merged_redir.size(); ++i) {
                            QuoteInfo qi_redir(merged_redir[i]);
                            const std::string& tok = qi_redir.value;
                            auto redir = parse_redirection_token(tok);
                            if (redir.has_value()) {
                                switch (*redir) {
                                    case RedirectionToken::StderrToStdout:
                                        cmd.stderr_to_stdout = true;
                                        break;
                                    case RedirectionToken::StdoutToStderr:
                                        cmd.stdout_to_stderr = true;
                                        break;
                                    case RedirectionToken::StderrOutput:
                                    case RedirectionToken::StderrAppend:
                                        if (i + 1 < merged_redir.size()) {
                                            cmd.stderr_file = QuoteInfo(merged_redir[++i]).value;
                                            cmd.stderr_append =
                                                (*redir == RedirectionToken::StderrAppend);
                                        }
                                        break;
                                    default:
                                        break;
                                }
                            }
                        }
                    }

                    commands.push_back(cmd);
                    continue;
                }
            }
        }

        std::vector<std::string> tokens = Tokenizer::tokenize_command(cmd_part);
        std::vector<std::string> filtered_args;

        auto is_all_digits = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(),
                                             [](unsigned char ch) { return std::isdigit(ch); });
        };

        auto handle_fd_duplication_token = [&](const std::string& token) {
            size_t pos = token.find("<&");
            char op = '<';
            if (pos == std::string::npos) {
                pos = token.find(">&");
                op = '>';
            }

            if (pos == std::string::npos) {
                return false;
            }

            std::string left = token.substr(0, pos);
            std::string right = token.substr(pos + 2);

            if (!right.empty() && right.front() == '$') {
                try {
                    expand_env_vars(right);
                } catch (const std::exception&) {
                    return false;
                }
            }

            if (right.empty()) {
                return false;
            }

            int dst_fd = (op == '>') ? 1 : 0;
            if (!left.empty()) {
                if (!is_all_digits(left)) {
                    return false;
                }
                dst_fd = std::stoi(left);
            }

            if (right == "-") {
                cmd.set_fd_duplication(dst_fd, -1);
                cmd.add_redirection(CommandRedirectionType::Close, "", dst_fd, -1);
                return true;
            }

            if (!is_all_digits(right)) {
                return false;
            }

            int src_fd = std::stoi(right);
            cmd.set_fd_duplication(dst_fd, src_fd);
            cmd.add_redirection(CommandRedirectionType::Duplicate, "", dst_fd, src_fd);
            return true;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            QuoteInfo qi(tokens[i]);
            auto redir = parse_redirection_token(qi.value);
            if (qi.is_unquoted() && redir.has_value() && redirection_requires_value(*redir) &&
                i + 1 >= tokens.size()) {
                throw std::runtime_error("cjsh: syntax error near unexpected token `newline'");
            }
        }

        auto get_next_token_value = [&tokens](size_t& i) { return QuoteInfo(tokens[++i]).value; };

        for (size_t i = 0; i < tokens.size(); ++i) {
            QuoteInfo qi(tokens[i]);
            if (!qi.is_unquoted()) {
                filtered_args.push_back(tokens[i]);
                continue;
            }

            auto redir = parse_redirection_token(qi.value);
            if (redir.has_value()) {
                switch (*redir) {
                    case RedirectionToken::Input:
                        cmd.input_file = get_next_token_value(i);
                        cmd.add_redirection(CommandRedirectionType::Input, cmd.input_file);
                        break;
                    case RedirectionToken::Output:
                        cmd.output_file = get_next_token_value(i);
                        cmd.force_overwrite = false;
                        cmd.add_redirection(CommandRedirectionType::Output, cmd.output_file);
                        break;
                    case RedirectionToken::Append:
                        cmd.append_file = get_next_token_value(i);
                        cmd.add_redirection(CommandRedirectionType::Append, cmd.append_file);
                        break;
                    case RedirectionToken::ForceOutput:
                        cmd.output_file = get_next_token_value(i);
                        cmd.force_overwrite = true;
                        cmd.add_redirection(CommandRedirectionType::ForceOutput, cmd.output_file);
                        break;
                    case RedirectionToken::BothOutput:
                        cmd.both_output_file = get_next_token_value(i);
                        cmd.both_output = true;
                        cmd.add_redirection(CommandRedirectionType::BothOutput,
                                            cmd.both_output_file);
                        break;
                    case RedirectionToken::HereDoc:
                    case RedirectionToken::HereDocStrip:
                        cmd.here_doc = get_next_token_value(i);
                        cmd.add_redirection(CommandRedirectionType::HereDoc, cmd.here_doc);
                        break;
                    case RedirectionToken::HereString:
                        cmd.here_string = get_next_token_value(i);
                        cmd.add_redirection(CommandRedirectionType::HereString, cmd.here_string);
                        break;
                    case RedirectionToken::ReadWrite:
                    case RedirectionToken::DupInput:
                    case RedirectionToken::DupOutput:
                        break;
                    case RedirectionToken::StderrOutput:
                    case RedirectionToken::StderrAppend:
                        cmd.stderr_file = get_next_token_value(i);
                        cmd.stderr_append = (*redir == RedirectionToken::StderrAppend);
                        cmd.add_redirection(cmd.stderr_append
                                                ? CommandRedirectionType::StderrAppend
                                                : CommandRedirectionType::StderrOutput,
                                            cmd.stderr_file);
                        break;
                    case RedirectionToken::StderrToStdout:
                        cmd.stderr_to_stdout = true;
                        cmd.add_redirection(CommandRedirectionType::Duplicate, "", STDERR_FILENO,
                                            STDOUT_FILENO);
                        break;
                    case RedirectionToken::StdoutToStderr:
                        cmd.stdout_to_stderr = true;
                        cmd.add_redirection(CommandRedirectionType::Duplicate, "", STDOUT_FILENO,
                                            STDERR_FILENO);
                        break;
                }
            } else if (handle_fd_duplication_token(qi.value)) {
                continue;
            } else if (handle_fd_redirection(qi.value, i, tokens, cmd, filtered_args)) {
                continue;
            } else {
                if ((qi.value.find("<(") == 0 && qi.value.back() == ')') ||
                    (qi.value.find(">(") == 0 && qi.value.back() == ')')) {
                    cmd.process_substitutions.push_back(qi.value);
                    filtered_args.push_back(tokens[i]);
                } else {
                    filtered_args.push_back(tokens[i]);
                }
            }
        }

        if (!filtered_args.empty()) {
            const std::string& alias_candidate = QuoteInfo(filtered_args[0]).value;
            auto alias_it = aliases.find(alias_candidate);
            if (alias_it != aliases.end()) {
                try {
                    std::vector<std::string> alias_args =
                        Tokenizer::tokenize_command(alias_it->second);

                    bool alias_has_pipe = false;
                    for (const auto& alias_arg : alias_args) {
                        if (QuoteInfo(alias_arg).value == "|") {
                            alias_has_pipe = true;
                            break;
                        }
                    }

                    if (!alias_args.empty() && !alias_has_pipe) {
                        size_t remaining_args =
                            filtered_args.size() > 1 ? filtered_args.size() - 1 : 0;
                        std::vector<std::string> new_args;
                        new_args.reserve(alias_args.size() + remaining_args);
                        (void)new_args.insert(new_args.end(), alias_args.begin(), alias_args.end());
                        if (filtered_args.size() > 1) {
                            (void)new_args.insert(new_args.end(), filtered_args.begin() + 1,
                                                  filtered_args.end());
                        }
                        filtered_args = std::move(new_args);
                    }
                } catch (const std::exception&) {
                    // Alias expansion is optional; keep original args on failure.
                }
            }
        }

        filtered_args = prepare_expansion_tokens(std::move(filtered_args));

        bool is_double_bracket_cmd =
            !filtered_args.empty() && QuoteInfo(filtered_args[0]).value == "[[";

        auto tilde_expanded_args = expand_tilde_tokens(filtered_args);

        std::vector<std::string> final_args_local;
        bool is_subshell_command =
            !filtered_args.empty() && QuoteInfo(filtered_args[0]).value == "__INTERNAL_SUBSHELL__";
        bool is_brace_group_command = !filtered_args.empty() && QuoteInfo(filtered_args[0]).value ==
                                                                    "__INTERNAL_BRACE_GROUP__";

        for (size_t arg_idx = 0; arg_idx < tilde_expanded_args.size(); ++arg_idx) {
            const auto& raw = tilde_expanded_args[arg_idx];
            QuoteInfo qi(raw);
            std::string& val = qi.value;
            bool had_noenv = val.find(noenv_start()) != std::string::npos;

            bool skip_env_expansion = (!qi.is_single) && ((is_subshell_command && arg_idx == 1) ||
                                                          (is_brace_group_command && arg_idx == 1));

            if (!qi.is_single && !skip_env_expansion) {
                if (!variableExpander) {
                    variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
                }
                try {
                    if (had_noenv) {
                        variableExpander->expand_env_vars_selective(val);
                    } else {
                        variableExpander->expand_env_vars(val);
                    }
                } catch (const std::runtime_error&) {
                    // Ignore optional env expansion failures; use unexpanded value.
                }
                (void)strip_subst_literal_markers(val);
                auto stripped_pair = strip_noenv_sentinels(val);
                val = std::move(stripped_pair.first);
            }

            bool is_assignment = looks_like_assignment(val);
            std::vector<std::string> fields;
            if (qi.is_unquoted() && !is_assignment) {
                if (!tokenizer) {
                    tokenizer = std::make_unique<Tokenizer>();
                }
                fields = tokenizer->split_by_ifs(val);
            } else {
                fields.push_back(val);
            }

            for (const auto& field : fields) {
                if (qi.is_unquoted() && !is_double_bracket_cmd && !is_assignment &&
                    requires_glob_expansion_or_unescape(field)) {
                    if (!expansionEngine) {
                        expansionEngine = std::make_unique<ExpansionEngine>(shell);
                    }
                    auto expanded = expansionEngine->expand_wildcards(field);
                    (void)final_args_local.insert(final_args_local.end(), expanded.begin(),
                                                  expanded.end());
                } else {
                    final_args_local.push_back(field);
                }
            }
        }

        cmd.args = final_args_local;

        if (!variableExpander) {
            variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
        }
        variableExpander->expand_command_redirection_paths(cmd);

        if (cjsh_env::shell_variable_is_set("HOME")) {
            if (!variableExpander) {
                variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
            }
            variableExpander->expand_command_paths_with_home(
                cmd, cjsh_env::get_shell_variable_value("HOME"));
        }

        commands.push_back(cmd);
    }

    if (pipeline_negated && !commands.empty()) {
        commands[0].negate_pipeline = true;
    }

    return commands;
}

std::vector<Command> Parser::parse_pipeline_with_preprocessing(const std::string& command) {
    // Preprocessing can replace heredoc placeholders retained by the input splitter.
    prepared_input.reset();
    auto preprocessed = CommandPreprocessor::preprocess(command);

    for (const auto& pair : preprocessed.here_documents) {
        current_here_docs[pair.first] = pair.second;
    }

    auto transform_group_marker = [&preprocessed](const std::string& marker,
                                                  const std::string& builtin_name) {
        const std::string& text = preprocessed.processed_text;
        size_t lead = text.find_first_not_of(" \t\r\n");
        if (lead == std::string::npos) {
            return;
        }

        if (text.compare(lead, marker.size(), marker) != 0) {
            return;
        }

        size_t brace_pos = lead + marker.size() - 1;
        size_t close_pos = find_matching_brace(text, brace_pos);
        if (close_pos == std::string::npos) {
            return;
        }

        size_t content_start = brace_pos + 1;
        std::string group_content = text.substr(content_start, close_pos - content_start);
        std::string remaining = text.substr(close_pos + 1);

        auto escape_double_quotes = [](const std::string& s) {
            std::string out;
            out.reserve(s.size() + 16);
            for (char c : s) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            return out;
        };

        std::string rebuilt =
            builtin_name + " \"" + escape_double_quotes(group_content) + "\"" + remaining;

        std::string prefix = text.substr(0, lead);
        preprocessed.processed_text = prefix + rebuilt;
    };

    transform_group_marker("SUBSHELL{", "__INTERNAL_SUBSHELL__");
    transform_group_marker("BRACEGROUP{", "__INTERNAL_BRACE_GROUP__");

    std::vector<Command> commands = parse_pipeline(preprocessed.processed_text);

    for (auto& cmd : commands) {
        if (!cmd.input_file.empty() && cmd.input_file.find("HEREDOC_PLACEHOLDER_") == 0) {
            std::string placeholder = cmd.input_file;
            auto it = current_here_docs.find(cmd.input_file);
            if (it != current_here_docs.end()) {
                std::string content = it->second;
                process_heredoc_content(content);
                cmd.here_doc = content;
                cmd.input_file.clear();
                for (auto& redirection : cmd.redirection_order) {
                    if (redirection.type == CommandRedirectionType::Input &&
                        redirection.value == placeholder) {
                        redirection.type = CommandRedirectionType::HereDoc;
                        redirection.value = cmd.here_doc;
                    }
                }
            }
        }

        if (!cmd.here_doc.empty() && (current_here_docs.count(cmd.here_doc) != 0U)) {
            std::string content = current_here_docs[cmd.here_doc];
            process_heredoc_content(content);
            cmd.here_doc = content;
        }

        if (!cmd.here_doc.empty()) {
            for (auto& redirection : cmd.redirection_order) {
                if (redirection.type == CommandRedirectionType::HereDoc) {
                    redirection.value = cmd.here_doc;
                }
            }
        }
    }

    return commands;
}

bool Parser::is_env_assignment(const std::string& command, std::string& var_name,
                               std::string& var_value) {
    if (!parse_env_assignment(command, var_name, var_value, true)) {
        return false;
    }

    if (!readonly_manager_can_assign(var_name, "parser")) {
        return false;
    }

    return true;
}

std::vector<LogicalCommand> Parser::parse_logical_commands(const std::string& command) {
    std::vector<LogicalCommand> logical_commands;
    // Without a logical operator the result is the original command, even when
    // it contains semicolons. Avoid scanning delimiters only to discard the split.
    if (command.find("&&") == std::string::npos && command.find("||") == std::string::npos) {
        if (!command.empty()) {
            logical_commands.push_back({command, ""});
        }
        return logical_commands;
    }

    std::string current;
    DelimiterState delimiters;
    int arith_depth = 0;
    int single_bracket_depth = 0;
    int parameter_brace_depth = 0;
    int control_depth = 0;
    bool has_logical_operator = false;

    for (size_t i = 0; i < command.length(); ++i) {
        if (delimiters.update_quote(command[i])) {
            current += command[i];
            continue;
        }

        if (!delimiters.in_quotes && command[i] == '(') {
            if (i >= 2 && command[i - 2] == '$' && command[i - 1] == '(' && command[i] == '(') {
                arith_depth++;
            }
            delimiters.paren_depth++;
            current += command[i];
            continue;
        }

        if (!delimiters.in_quotes && command[i] == ')') {
            delimiters.paren_depth--;

            if (delimiters.paren_depth >= 0 && i + 1 < command.length() && command[i + 1] == ')' &&
                arith_depth > 0) {
                arith_depth--;
                current += command[i];
                current += command[i + 1];
                i++;
                continue;
            } else {
                current += command[i];
                continue;
            }
        }

        if (!delimiters.in_quotes && command[i] == '[') {
            size_t increment = 0;
            if (handle_double_bracket_open(command, i, current, delimiters, increment)) {
                i += increment;
                continue;
            }

            bool is_test_bracket = (i == 0 || command[i - 1] == ' ' || command[i - 1] == '\t' ||
                                    command[i - 1] == ';' || command[i - 1] == '\n');
            if (is_test_bracket) {
                single_bracket_depth++;
            }
            current += command[i];
            continue;
        }

        if (!delimiters.in_quotes && command[i] == ']') {
            size_t increment = 0;
            if (handle_double_bracket_close(command, i, current, delimiters, increment)) {
                i += increment;
                continue;
            }

            if (single_bracket_depth > 0) {
                bool is_test_close =
                    (i + 1 >= command.length() || command[i + 1] == ' ' || command[i + 1] == '\t' ||
                     command[i + 1] == ';' || command[i + 1] == '&' || command[i + 1] == '|' ||
                     command[i + 1] == '\n');
                if (is_test_close) {
                    single_bracket_depth--;
                }
            }
            current += command[i];
            continue;
        }

        if (!delimiters.in_quotes && command[i] == '{') {
            bool starts_parameter_expansion =
                i > 0 && command[i - 1] == '$' && !is_char_escaped(command, i - 1);
            if (parameter_brace_depth > 0 || starts_parameter_expansion) {
                parameter_brace_depth++;
            } else if (parser_is_command_group_brace(command, i)) {
                delimiters.brace_depth++;
            }
            current += command[i];
            continue;
        }

        if (!delimiters.in_quotes && command[i] == '}') {
            if (parameter_brace_depth > 0) {
                parameter_brace_depth--;
            } else if (delimiters.brace_depth > 0 && parser_is_command_group_brace(command, i)) {
                delimiters.brace_depth--;
            }
            current += command[i];
            continue;
        }

        (void)is_control_word_at_position(command, i, delimiters.paren_depth,
                                          delimiters.brace_depth, delimiters.in_quotes,
                                          control_depth);

        if (!delimiters.in_quotes && delimiters.paren_depth == 0 && arith_depth == 0 &&
            delimiters.bracket_depth == 0 && delimiters.brace_depth == 0 &&
            single_bracket_depth == 0 && parameter_brace_depth == 0 && control_depth == 0) {
            if (command[i] == ';' && !is_char_escaped(command, i)) {
                if (!current.empty()) {
                    logical_commands.push_back({current, ""});
                    current.clear();
                }
                continue;
            }
            if (i < command.length() - 1 && command[i] == '&' && command[i + 1] == '&') {
                has_logical_operator = true;
                if (!current.empty()) {
                    logical_commands.push_back({current, "&&"});
                    current.clear();
                }
                i++;
                continue;
            }
            if (i < command.length() - 1 && command[i] == '|' && command[i + 1] == '|') {
                has_logical_operator = true;
                if (!current.empty()) {
                    logical_commands.push_back({current, "||"});
                    current.clear();
                }
                i++;
                continue;
            }
        }

        current += command[i];
    }

    if (!current.empty()) {
        logical_commands.push_back({current, ""});
    }

    if (!has_logical_operator) {
        logical_commands.clear();
        if (!command.empty()) {
            logical_commands.push_back({command, ""});
        }
    }

    return logical_commands;
}

std::vector<std::string> Parser::parse_semicolon_commands(const std::string& command,
                                                          bool split_on_newlines) {
    std::vector<std::string> commands;
    if (command.find(';') == std::string::npos &&
        (!split_on_newlines || command.find('\n') == std::string::npos)) {
        std::string trimmed = trim_whitespace(command);
        if (!trimmed.empty()) {
            commands.push_back(std::move(trimmed));
        }
        return commands;
    }

    std::string current;
    DelimiterState scan_state;
    int control_depth = 0;

    std::vector<bool> is_semicolon_split_point(command.length(), false);
    std::vector<bool> is_newline_split_point;
    if (split_on_newlines) {
        is_newline_split_point.assign(command.length(), false);
    }

    for (size_t i = 0; i < command.length(); ++i) {
        if (scan_state.update_quote(command[i])) {
            continue;
        }
        if (!scan_state.in_quotes && command[i] == '(') {
            scan_state.paren_depth++;
        } else if (!scan_state.in_quotes && command[i] == ')') {
            scan_state.paren_depth--;
        } else if (!scan_state.in_quotes && command[i] == '{') {
            scan_state.brace_depth++;
        } else if (!scan_state.in_quotes && command[i] == '}') {
            scan_state.brace_depth--;
        }

        (void)is_control_word_at_position(command, i, scan_state.paren_depth,
                                          scan_state.brace_depth, scan_state.in_quotes,
                                          control_depth);

        if (!scan_state.in_quotes && scan_state.paren_depth == 0 && scan_state.brace_depth == 0) {
            if ((split_on_newlines && command[i] == '\n' && control_depth == 0) &&
                (!is_newline_split_point.empty())) {
                is_newline_split_point[i] = true;
            }

            if ((command[i] == ';' && control_depth == 0) && (!is_char_escaped(command, i)))
            // only split at top level so semicolons inside if/then/fi headers stay intact
            {
                is_semicolon_split_point[i] = true;
            }
        }
    }

    DelimiterState parse_state;
    current.clear();

    for (size_t i = 0; i < command.length(); ++i) {
        if (parse_state.update_quote(command[i])) {
            current += command[i];
        } else if ((command[i] == ';' && is_semicolon_split_point[i]) ||
                   (split_on_newlines && !parse_state.in_quotes && command[i] == '\n' &&
                    (is_newline_split_point.empty() || is_newline_split_point[i]))) {
            if (!current.empty()) {
                current = trim_trailing_whitespace(trim_leading_whitespace(current));
                if (!current.empty()) {
                    commands.push_back(current);
                }
                current.clear();
            }
        } else {
            current += command[i];
        }
    }

    if (!current.empty()) {
        current = trim_trailing_whitespace(trim_leading_whitespace(current));
        if (!current.empty()) {
            commands.push_back(current);
        }
    }

    return commands;
}

std::vector<std::string> Parser::expand_wildcards(const std::string& pattern) {
    if (!expansionEngine) {
        expansionEngine = std::make_unique<ExpansionEngine>(shell);
    }
    return expansionEngine->expand_wildcards(pattern);
}

void Parser::expand_env_vars(std::string& arg) {
    if (!variableExpander) {
        variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
    }
    variableExpander->expand_env_vars(arg);
}

void Parser::expand_env_vars_selective(std::string& arg) {
    if (!variableExpander) {
        variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
    }
    variableExpander->expand_env_vars_selective(arg);
}

void Parser::expand_exported_env_vars_only(std::string& arg) {
    if (!variableExpander) {
        variableExpander = std::make_unique<VariableExpander>(shell, env_vars);
    }
    variableExpander->expand_exported_env_vars_only(arg);
}

std::vector<std::string> Parser::split_by_ifs(const std::string& input) {
    if (!tokenizer) {
        tokenizer = std::make_unique<Tokenizer>();
    }
    return tokenizer->split_by_ifs(input);
}

long long Parser::evaluate_arithmetic(const std::string& expr) {
    std::string trimmed = trim_whitespace(expr);
    if (trimmed.empty()) {
        return 0;
    }

    auto find_operator = [](const std::string& s, const std::string& op) -> size_t {
        int paren_depth = 0;
        for (size_t i = 0; i <= s.length() - op.length(); ++i) {
            if (s[i] == '(') {
                paren_depth++;
            } else if (s[i] == ')') {
                paren_depth--;
            } else if (paren_depth == 0 && s.substr(i, op.length()) == op) {
                if (op.length() == 1) {
                    if ((op == "+" || op == "-") && i > 0 && s[i - 1] == op[0]) {
                        continue;
                    }
                    if ((op == "+" || op == "-") && i + 1 < s.length() && s[i + 1] == op[0]) {
                        continue;
                    }
                }
                return i;
            }
        }
        return std::string::npos;
    };

    auto parse_number = [](const std::string& s) -> long long {
        std::string clean = trim_whitespace(s);
        if (clean.empty()) {
            return 0;
        }
        try {
            return std::stoll(clean);
        } catch (...) {
            return 0;
        }
    };

    size_t paren_start = trimmed.find('(');
    if (paren_start != std::string::npos) {
        int depth = 1;
        size_t paren_end = paren_start + 1;
        while (paren_end < trimmed.length() && depth > 0) {
            if (trimmed[paren_end] == '(') {
                depth++;
            } else if (trimmed[paren_end] == ')') {
                depth--;
            }
            paren_end++;
        }
        if (depth == 0) {
            std::string inner = trimmed.substr(paren_start + 1, paren_end - paren_start - 2);
            long long inner_result = evaluate_arithmetic(inner);
            std::string new_expr = trimmed.substr(0, paren_start) + std::to_string(inner_result) +
                                   trimmed.substr(paren_end);
            return evaluate_arithmetic(new_expr);
        }
    }

    std::vector<std::string> operators = {"+", "-", "*", "/", "%"};

    for (const auto& op : operators) {
        size_t pos = find_operator(trimmed, op);
        if (pos != std::string::npos) {
            std::string left = trimmed.substr(0, pos);
            std::string right = trimmed.substr(pos + op.length());

            long long left_val = evaluate_arithmetic(left);
            long long right_val = evaluate_arithmetic(right);

            switch (op[0]) {
                case '+':
                    return left_val + right_val;
                case '-':
                    return left_val - right_val;
                case '*':
                    return left_val * right_val;
                case '/':
                    if (right_val == 0) {
                        throw std::runtime_error("Division by zero");
                    }
                    return left_val / right_val;
                case '%':
                    if (right_val == 0) {
                        throw std::runtime_error("Division by zero");
                    }
                    return left_val % right_val;
            }
        }
    }

    return parse_number(trimmed);
}
