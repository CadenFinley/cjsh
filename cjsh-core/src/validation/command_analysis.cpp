/*
  command_analysis.cpp

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

#include "command_analysis.h"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "interpreter_utils.h"
#include "parser_utils.h"
#include "quote_info.h"
#include "quote_state.h"
#include "shell.h"
#include "shell_env.h"
#include "token_classifier.h"
#include "tokenizer.h"

namespace command_analysis {

bool extract_next_token(const std::string& cmd, size_t& cursor, size_t& token_start,
                        size_t& token_end) {
    const size_t len = cmd.length();

    while (cursor < len && (std::isspace(static_cast<unsigned char>(cmd[cursor])) != 0)) {
        ++cursor;
    }

    if (cursor >= len) {
        return false;
    }

    size_t start = cursor;
    cursor = find_token_end_with_quotes(cmd, start, len, "", true);

    token_start = start;
    token_end = cursor;
    return true;
}

bool token_has_explicit_path_hint(const std::string& token) {
    return cjsh_filesystem::token_has_explicit_path_hint(token);
}

std::string resolve_token_path(const std::string& token, const Shell* shell) {
    std::string path_token = token;
    if (token.find_first_of("\\\"'") != std::string::npos) {
        // Decode only shell quoting here; highlighting must not execute expansions.
        try {
            const auto words = Tokenizer::tokenize_command(token);
            if (words.size() == 1) {
                path_token = QuoteInfo(words.front()).unescaped_value();
                // A quoted or escaped leading tilde names a literal relative path.
                if (!path_token.empty() && path_token.front() == '~' && token.front() != '~') {
                    path_token.insert(0, "./");
                }
            }
        } catch (const std::runtime_error&) {
            // Incomplete quotes are common while editing; retain the original token.
        }
    }
    const std::string previous_directory =
        (shell != nullptr) ? shell->get_previous_directory() : "";
    return cjsh_filesystem::resolve_shell_token_path(
        path_token, cjsh_filesystem::safe_current_directory(), previous_directory);
}

bool token_is_history_expansion(const std::string& token, size_t absolute_cmd_start) {
    if (!config::history_expansion_enabled || token.empty()) {
        return false;
    }

    if (token[0] == '!') {
        return true;
    }

    if (token[0] == '^' && absolute_cmd_start == 0) {
        return true;
    }

    return false;
}

CommandTokenClassification classify_command_token(
    const std::string& token, size_t absolute_cmd_start, Shell* shell,
    const std::unordered_set<std::string>& available_commands) {
    using namespace token_classifier;
    using Kind = CommandTokenKind;
    if (token.empty()) {
        return {Kind::Empty, true};
    }
    if (is_variable_reference(token)) {
        return {Kind::Variable, true};
    }
    if (token_is_history_expansion(token, absolute_cmd_start)) {
        return {Kind::HistoryExpansion, true};
    }
    if (token_has_explicit_path_hint(token)) {
        std::error_code ec;
        return {Kind::ExplicitPath, std::filesystem::exists(resolve_token_path(token, shell), ec)};
    }
    if (shell != nullptr && shell->get_interactive_mode() &&
        shell->get_abbreviations().count(token) != 0) {
        return {Kind::Abbreviation, true};
    }
    if (command_lookup::is_shell_keyword(token)) {
        return {Kind::Keyword, true};
    }
    if (command_lookup::is_shell_builtin(token, shell)) {
        return {Kind::Builtin, true};
    }

    bool directory = false;
    if (command_lookup::should_auto_cd_token(token, shell, &directory)) {
        return {Kind::Directory, true};
    }

    const bool available = available_commands.count(token) != 0 ||
                           (shell != nullptr && shell->get_aliases().count(token) != 0) ||
                           command_lookup::has_shell_function(token, shell);
    const bool known = available || is_external_command(token);
    // Directories retain their path style even when an alias/function/executable takes
    // precedence over automatic cd during command execution.
    return {directory   ? Kind::Directory
            : available ? Kind::AvailableCommand
            : known     ? Kind::External
                        : Kind::Unknown,
            known};
}

bool is_known_command_token(const std::string& token, size_t absolute_cmd_start, Shell* shell,
                            const std::unordered_set<std::string>& available_commands) {
    return classify_command_token(token, absolute_cmd_start, shell, available_commands).known;
}

std::string sanitize_input_for_analysis(const std::string& input,
                                        std::vector<CommentRange>* comment_ranges) {
    std::string sanitized = input;
    const size_t len = input.size();
    size_t line_start = 0;
    while (line_start < len) {
        size_t line_end = line_start;
        while (line_end < len && input[line_end] != '\n' && input[line_end] != '\r') {
            ++line_end;
        }

        size_t comment_start = shell_script_interpreter::detail::find_inline_comment_start(
            input, line_start, line_end);
        if (comment_start != std::string::npos) {
            for (size_t i = comment_start; i < line_end; ++i) {
                sanitized[i] = ' ';
            }
            if (comment_ranges != nullptr && line_end > comment_start) {
                comment_ranges->push_back({comment_start, line_end});
            }
        }

        if (line_end >= len) {
            break;
        }

        if (input[line_end] == '\r' && line_end + 1 < len && input[line_end + 1] == '\n') {
            line_start = line_end + 2;
        } else {
            line_start = line_end + 1;
        }
    }

    return sanitized;
}

CommandSeparator scan_command_separator(const std::string& analysis, size_t index) {
    CommandSeparator match;
    const size_t len = analysis.size();
    if (index >= len) {
        return match;
    }

    char current = analysis[index];
    if ((current == '>' || current == '<') && index + 1 < len && analysis[index + 1] == '&') {
        return match;
    }
    if (index + 2 < len && current == '&' && analysis[index + 1] == '^' &&
        analysis[index + 2] == '!') {
        match.length = 3;
        match.is_operator = true;
        return match;
    }

    if (index + 1 < len) {
        char next = analysis[index + 1];
        if ((current == '&' && next == '&') || (current == '&' && next == '^') ||
            (current == '|' && next == '|') || (current == '>' && next == '>') ||
            (current == '<' && next == '<') || (current == '&' && next == '>')) {
            match.length = 2;
            match.is_operator = true;
            return match;
        }
        if (current == '\r' && next == '\n') {
            match.length = 2;
            match.is_operator = false;
            return match;
        }
    }

    if (current == '|' || current == ';' || current == '>' || current == '<') {
        match.length = 1;
        match.is_operator = true;
        return match;
    }

    if (current == '&' && (index == len - 1 || analysis[index + 1] != '&')) {
        if (index > 0 && (analysis[index - 1] == '>' || analysis[index - 1] == '<')) {
            return match;
        }
        match.length = 1;
        match.is_operator = true;
        return match;
    }

    if (current == '\n' || current == '\r') {
        match.length = 1;
        match.is_operator = false;
        return match;
    }

    return match;
}

bool separator_is_redirection_operator(const std::string& analysis, size_t separator_start,
                                       const CommandSeparator& separator) {
    if (!separator.is_operator || separator.length == 0 || separator_start >= analysis.size()) {
        return false;
    }

    const char first = analysis[separator_start];
    if (first == '>' || first == '<') {
        return true;
    }

    return separator.length >= 2 && first == '&' && separator_start + 1 < analysis.size() &&
           analysis[separator_start + 1] == '>';
}

size_t find_command_end(const std::string& analysis, size_t start) {
    const size_t len = analysis.size();
    size_t cmd_end = start;
    utils::QuoteState cmd_quote_state;
    int arithmetic_context_depth = 0;
    while (cmd_end < len) {
        char current = analysis[cmd_end];
        auto action = cmd_quote_state.consume_forward(current);
        if (action == utils::QuoteAdvanceResult::Process && !cmd_quote_state.inside_quotes()) {
            if (cmd_end + 1 < len && analysis.compare(cmd_end, 2, "((") == 0) {
                ++arithmetic_context_depth;
                cmd_end += 2;
                continue;
            }

            if (cmd_end + 1 < len && arithmetic_context_depth > 0 &&
                analysis.compare(cmd_end, 2, "))") == 0) {
                --arithmetic_context_depth;
                cmd_end += 2;
                continue;
            }

            if (arithmetic_context_depth == 0) {
                auto separator = scan_command_separator(analysis, cmd_end);
                if (separator.length > 0) {
                    break;
                }
            }
        }
        cmd_end++;
    }

    return cmd_end;
}

bool visit_command_ranges(
    const std::string& analysis,
    const std::function<bool(size_t command_start, size_t command_end)>& visit_command,
    const std::function<void(size_t separator_start, const CommandSeparator&)>& visit_separator) {
    const size_t length = analysis.size();
    size_t position = 0;
    bool next_range_is_redirection_operand = false;

    while (position < length) {
        const size_t command_end = find_command_end(analysis, position);
        size_t command_start = position;
        while (command_start < command_end &&
               std::isspace(static_cast<unsigned char>(analysis[command_start])) != 0) {
            ++command_start;
        }

        if (command_start < command_end && !next_range_is_redirection_operand && visit_command &&
            !visit_command(command_start, command_end)) {
            return false;
        }

        position = command_end;
        if (position >= length) {
            break;
        }

        const auto separator = scan_command_separator(analysis, position);
        if (separator.length == 0) {
            next_range_is_redirection_operand = false;
            ++position;
            continue;
        }

        if (visit_separator) {
            visit_separator(position, separator);
        }
        next_range_is_redirection_operand =
            separator_is_redirection_operator(analysis, position, separator);
        position += separator.length;
    }

    return true;
}

}  // namespace command_analysis
