/*
  cjsh_syntax_highlighter.cpp

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

#include "cjsh_syntax_highlighter.h"
#include <algorithm>
#include <cstdint>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agent_mode.h"
#include "builtin.h"
#include "cjsh_filesystem.h"
#include "command_analysis.h"
#include "command_lookup.h"
#include "highlight_helpers.h"
#include "isocline.h"
#include "shell.h"
#include "shell_env.h"
#include "token_classifier.h"
#include "token_constants.h"

namespace {

bool is_grouping_delimiter_token(const std::string& token) {
    return token == "(" || token == ")" || token == "{" || token == "}";
}

bool is_opening_grouping_delimiter_token(const std::string& token) {
    return token == "(" || token == "{";
}

enum class ExistingPathType : std::uint8_t {
    None,
    RegularFile,
    Other
};

constexpr size_t kMaxHighlightCacheEntries = 64;

struct HighlightPathContext {
    std::optional<std::string> cwd;
    std::string previous_directory;
    std::optional<std::vector<std::string>> executables;
    std::optional<std::unordered_set<std::string>> commands;
    // A redraw sees one snapshot; never retain filesystem or shell-state answers
    // across callbacks. Repeated words in multiline input need only one lookup.
    // Cap each cache so large inputs with unique words do not allocate per token.
    std::unordered_map<std::string, command_analysis::CommandTokenClassification> classifications;
    std::unordered_map<std::string, ExistingPathType> argument_paths;
    std::unordered_map<std::string, bool> split_candidates;

    command_analysis::CommandTokenClassification classify(const std::string& token,
                                                          size_t absolute_start) {
        // Quick history substitution is only recognized at the start of the input.
        if (!token.empty() && token.front() == '^') {
            return command_analysis::classify_command_token(token, absolute_start, g_shell.get());
        }
        const auto found = classifications.find(token);
        if (found != classifications.end()) {
            return found->second;
        }
        const auto result =
            command_analysis::classify_command_token(token, absolute_start, g_shell.get());
        if (classifications.size() < kMaxHighlightCacheEntries) {
            classifications.emplace(token, result);
        }
        return result;
    }

    void initialize() {
        if (!cwd.has_value()) {
            cwd = cjsh_filesystem::safe_current_directory();
            previous_directory = g_shell ? g_shell->get_previous_directory() : "";
        }
    }

    const std::unordered_set<std::string>& available_commands() {
        if (!commands.has_value()) {
            commands =
                g_shell ? g_shell->get_available_commands() : std::unordered_set<std::string>{};
        }
        return *commands;
    }

    const std::vector<std::string>& executables_in_path() {
        if (!executables.has_value()) {
            executables = cjsh_filesystem::get_path_completion_candidates();
        }
        return *executables;
    }
};

ExistingPathType classify_existing_path_argument(const std::string& token,
                                                 HighlightPathContext& paths) {
    if (token.empty() || token == "-") {
        return ExistingPathType::None;
    }

    const auto found = paths.argument_paths.find(token);
    if (found != paths.argument_paths.end()) {
        return found->second;
    }
    paths.initialize();
    const std::string path_to_check =
        cjsh_filesystem::resolve_shell_token_path(token, *paths.cwd, paths.previous_directory);
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::status(path_to_check, status_error);
    const auto result = status_error || !std::filesystem::exists(status) ? ExistingPathType::None
                        : std::filesystem::is_regular_file(status) ? ExistingPathType::RegularFile
                                                                   : ExistingPathType::Other;
    if (paths.argument_paths.size() < kMaxHighlightCacheEntries) {
        paths.argument_paths.emplace(token, result);
    }
    return result;
}

bool has_nearby_split_merge_candidate(const std::string& first_token,
                                      const std::string& second_token,
                                      HighlightPathContext& paths) {
    if (first_token.length() < 2 || second_token.length() < 2) {
        return false;
    }

    // Include the boundary: different word splits can have the same concatenation.
    const std::string key = first_token + '\0' + second_token;
    const auto found = paths.split_candidates.find(key);
    if (found != paths.split_candidates.end()) {
        return found->second;
    }
    constexpr size_t kMaxGapChars = 1;
    const size_t min_candidate_length = first_token.length() + second_token.length();
    const size_t max_candidate_length = min_candidate_length + kMaxGapChars;

    auto matches_candidate = [&](const std::string& candidate) {
        if (candidate.length() < min_candidate_length ||
            candidate.length() > max_candidate_length) {
            return false;
        }

        if (candidate.rfind(first_token, 0) != 0) {
            return false;
        }

        return candidate.compare(candidate.length() - second_token.length(), second_token.length(),
                                 second_token) == 0;
    };

    for (const auto& candidate : paths.available_commands()) {
        if (matches_candidate(candidate)) {
            if (paths.split_candidates.size() < kMaxHighlightCacheEntries) {
                paths.split_candidates.emplace(key, true);
            }
            return true;
        }
    }

    const auto& executables = paths.executables_in_path();
    const bool result =
        std::any_of(executables.begin(), executables.end(), [&](const std::string& candidate) {
            return matches_candidate(candidate) &&
                   !cjsh_filesystem::find_executable_in_path(candidate).empty();
        });
    if (paths.split_candidates.size() < kMaxHighlightCacheEntries) {
        paths.split_candidates.emplace(key, result);
    }
    return result;
}

void highlight_command_range(ic_highlight_env_t* henv, const char* input,
                             const std::string& analysis, size_t cmd_start, size_t cmd_end,
                             const std::unordered_set<std::string>& comparison_ops,
                             HighlightPathContext& paths) {
    using namespace token_classifier;
    using namespace highlight_helpers;

    if (cmd_start >= cmd_end) {
        return;
    }

    std::string cmd_str(analysis.c_str() + cmd_start, cmd_end - cmd_start);

    size_t token_cursor = 0;
    size_t first_token_start = 0;
    size_t first_token_end = 0;
    if (!command_analysis::extract_next_token(cmd_str, token_cursor, first_token_start,
                                              first_token_end)) {
        return;
    }

    std::string token = cmd_str.substr(first_token_start, first_token_end - first_token_start);
    bool is_sudo_command = (token == "sudo");
    size_t absolute_token_start = cmd_start + first_token_start;
    size_t first_token_length = first_token_end - first_token_start;

    const auto classification = paths.classify(token, absolute_token_start);
    const bool first_token_unknown = !classification.known;

    bool highlight_split_unknown_second_token = false;
    size_t split_second_token_absolute_start = 0;
    size_t split_second_token_length = 0;
    if (first_token_unknown) {
        size_t split_cursor = token_cursor;
        size_t second_token_start = 0;
        size_t second_token_end = 0;
        if (command_analysis::extract_next_token(cmd_str, split_cursor, second_token_start,
                                                 second_token_end)) {
            size_t third_token_start = 0;
            size_t third_token_end = 0;
            const bool has_third_token = command_analysis::extract_next_token(
                cmd_str, split_cursor, third_token_start, third_token_end);
            if (!has_third_token && command_lookup::token_allows_split_command_merge(token)) {
                std::string second_token =
                    cmd_str.substr(second_token_start, second_token_end - second_token_start);
                if (command_lookup::token_allows_split_command_merge(second_token)) {
                    const size_t absolute_second_token_start = cmd_start + second_token_start;
                    const bool merged_token_known =
                        paths.classify(token + second_token, absolute_token_start).known;
                    const bool merged_token_near_match =
                        !merged_token_known &&
                        has_nearby_split_merge_candidate(token, second_token, paths);

                    if (merged_token_known || merged_token_near_match) {
                        highlight_split_unknown_second_token = true;
                        split_second_token_absolute_start = absolute_second_token_start;
                        split_second_token_length = second_token_end - second_token_start;
                    }
                }
            }
        }
    }

    const char* command_style = nullptr;
    using Kind = command_analysis::CommandTokenKind;
    if (is_grouping_delimiter_token(token)) {
        command_style = "cjsh-operator";
    } else {
        switch (classification.kind) {
            case Kind::Variable:
                highlight_variable_assignment(henv, input, absolute_token_start, token);
                break;
            case Kind::Empty:
            case Kind::HistoryExpansion:
                break;
            case Kind::ExplicitPath:
            case Kind::External:
                command_style = classification.known ? "cjsh-system" : "cjsh-unknown-command";
                break;
            case Kind::Abbreviation:
            case Kind::Builtin:
            case Kind::AvailableCommand:
                command_style = "cjsh-builtin";
                break;
            case Kind::Keyword:
                command_style = "cjsh-keyword";
                break;
            case Kind::Directory:
                command_style = "cjsh-path-exists";
                break;
            case Kind::Unknown:
                command_style = "cjsh-unknown-command";
                break;
        }
    }
    if (command_style != nullptr) {
        ic_highlight(henv, static_cast<long>(absolute_token_start),
                     static_cast<long>(first_token_length), command_style);
    }

    bool recurse_into_nested_command = (token_constants::inline_command_keywords().find(token) !=
                                        token_constants::inline_command_keywords().end()) ||
                                       is_opening_grouping_delimiter_token(token);

    if (recurse_into_nested_command) {
        size_t nested_start = first_token_end;
        while (nested_start < cmd_str.size() &&
               (std::isspace(static_cast<unsigned char>(cmd_str[nested_start])) != 0)) {
            nested_start++;
        }
        if (nested_start < cmd_str.size()) {
            highlight_command_range(henv, input, analysis, cmd_start + nested_start, cmd_end,
                                    comparison_ops, paths);
        }
        return;
    }

    bool is_cd_command = (token == "cd");
    size_t arg_cursor = token_cursor;
    size_t arg_index = 0;
    size_t arg_start = 0;
    size_t arg_end = 0;

    while (command_analysis::extract_next_token(cmd_str, arg_cursor, arg_start, arg_end)) {
        size_t absolute_arg_start = cmd_start + arg_start;
        size_t arg_length = arg_end - arg_start;
        std::string arg = cmd_str.substr(arg_start, arg_length);

        if (highlight_split_unknown_second_token && arg_index == 0 &&
            absolute_arg_start == split_second_token_absolute_start &&
            arg_length == split_second_token_length) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-unknown-command");
            ++arg_index;
            continue;
        }

        if (is_redirection_operator(arg) || comparison_ops.count(arg) > 0) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-operator");
        }

        else if (is_variable_reference(arg)) {
            highlight_variable_assignment(henv, input, absolute_arg_start, arg);
        }

        else if (is_grouping_delimiter_token(arg)) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-operator");
        }

        else if (arg == "((" || arg == "))") {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-arithmetic");
        }

        else if (is_shell_keyword(arg)) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-keyword");
        }

        else if (is_option(arg)) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-option");
        }

        else if (is_numeric_literal(arg)) {
            ic_highlight(henv, static_cast<long>(absolute_arg_start), static_cast<long>(arg_length),
                         "cjsh-number");
        }

        else {
            char quote_type = 0;
            if (is_quoted_string(arg, quote_type)) {
                ic_highlight(henv, static_cast<long>(absolute_arg_start),
                             static_cast<long>(arg_length), "cjsh-string");
            } else if (is_sudo_command && arg_index == 0) {
                if (!command_analysis::token_is_history_expansion(arg, absolute_arg_start)) {
                    if (arg.rfind("./", 0) == 0) {
                        if (!std::filesystem::exists(arg) ||
                            !std::filesystem::is_regular_file(arg)) {
                            ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                         static_cast<long>(arg_length), "cjsh-unknown-command");
                        } else {
                            ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                         static_cast<long>(arg_length), "cjsh-system");
                        }
                    } else {
                        bool is_abbreviation = false;
                        if (g_shell != nullptr && g_shell->get_interactive_mode()) {
                            const auto& abbreviations = g_shell->get_abbreviations();
                            is_abbreviation = abbreviations.find(arg) != abbreviations.end();
                        }

                        if (is_abbreviation ||
                            (g_shell && g_shell->get_aliases().count(arg) != 0) ||
                            command_lookup::has_shell_function(arg, g_shell.get()) ||
                            is_shell_builtin(arg)) {
                            ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                         static_cast<long>(arg_length), "cjsh-builtin");
                        } else if (is_external_command(arg)) {
                            ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                         static_cast<long>(arg_length), "cjsh-system");
                        } else {
                            ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                         static_cast<long>(arg_length), "cjsh-unknown-command");
                        }
                    }
                }
            } else if (is_cd_command && (arg == "~" || arg == "-")) {
                ic_highlight(henv, static_cast<long>(absolute_arg_start),
                             static_cast<long>(arg_length), "cjsh-path-exists");
            } else if (is_glob_pattern(arg)) {
                ic_highlight(henv, static_cast<long>(absolute_arg_start),
                             static_cast<long>(arg_length), "cjsh-glob-pattern");
            } else if (is_cd_command || command_analysis::token_has_explicit_path_hint(arg)) {
                const ExistingPathType path_type = classify_existing_path_argument(arg, paths);
                if (path_type == ExistingPathType::RegularFile) {
                    ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                 static_cast<long>(arg_length), "cjsh-file-argument");
                } else if (path_type == ExistingPathType::Other) {
                    ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                 static_cast<long>(arg_length), "cjsh-path-exists");
                } else {
                    ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                 static_cast<long>(arg_length), "cjsh-path-not-exists");
                }
            } else {
                const ExistingPathType path_type = classify_existing_path_argument(arg, paths);
                if (path_type == ExistingPathType::RegularFile) {
                    ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                 static_cast<long>(arg_length), "cjsh-file-argument");
                } else if (path_type == ExistingPathType::Other) {
                    ic_highlight(henv, static_cast<long>(absolute_arg_start),
                                 static_cast<long>(arg_length), "cjsh-path-exists");
                }
            }
        }

        if (!is_variable_reference(arg) &&
            (arg.find('$') != std::string::npos || arg.find('`') != std::string::npos)) {
            highlight_quotes_and_variables(henv, input, absolute_arg_start, arg_length);
        }

        ++arg_index;
    }
}

}  // namespace

void SyntaxHighlighter::initialize_syntax_highlighting() {
    if (config::syntax_highlighting_enabled) {
        ic_set_default_highlighter(SyntaxHighlighter::highlight, nullptr);
        (void)ic_enable_highlight(true);
    } else {
        ic_set_default_highlighter(nullptr, nullptr);
        (void)ic_enable_highlight(false);
    }
}

void SyntaxHighlighter::highlight(ic_highlight_env_t* henv, const char* input, void*) {
    const cjsh_filesystem::ScopedInteractivePathLookup path_lookup;
    using namespace token_classifier;
    using namespace highlight_helpers;
    using namespace token_constants;

    size_t len = std::strlen(input);
    if (len == 0) {
        return;
    }

    std::string raw_input(input, len);

    const auto agent_prefix_length = agent_mode::matching_trigger_prefix_length(raw_input);
    if (agent_prefix_length.has_value()) {
        ic_highlight(henv, 0L, static_cast<long>(*agent_prefix_length), "cjsh-agent-prefix");
        if (*agent_prefix_length < len) {
            ic_highlight(henv, static_cast<long>(*agent_prefix_length),
                         static_cast<long>(len - *agent_prefix_length), "cjsh-agent-request");
        }
        return;
    }

    const auto heredoc_ranges = find_heredoc_ranges(input, len);
    // Keep heredoc text out of command/variable/comment classification. In
    // particular, closing markers must not inherit unknown-command underlines.
    for (const auto& range : heredoc_ranges) {
        for (size_t i = range.start; i < range.end; ++i) {
            if (raw_input[i] != '\n') {
                raw_input[i] = ' ';
            }
        }
    }
    input = raw_input.c_str();
    const auto highlight_heredocs = [&] {
        for (const auto& range : heredoc_ranges) {
            ic_highlight(henv, static_cast<long>(range.start),
                         static_cast<long>(range.end - range.start),
                         range.is_delimiter ? "cjsh-heredoc-delimiter" : "cjsh-string");
        }
    };

    std::vector<command_analysis::CommentRange> comment_ranges;
    std::string sanitized_input =
        command_analysis::sanitize_input_for_analysis(raw_input, &comment_ranges);

    if (config::history_expansion_enabled) {
        highlight_history_expansions(henv, input, len);
    }

    size_t func_name_start = 0;
    size_t func_name_end = 0;
    if (is_function_definition(sanitized_input, func_name_start, func_name_end)) {
        ic_highlight(henv, static_cast<long>(func_name_start),
                     static_cast<long>(func_name_end - func_name_start),
                     "cjsh-function-definition");

        size_t paren_pos = sanitized_input.find("()", func_name_end);
        if (paren_pos != std::string::npos && paren_pos < len) {
            ic_highlight(henv, static_cast<long>(paren_pos), 2L, "cjsh-function-definition");
        }

        size_t brace_pos = sanitized_input.find('{');
        if (brace_pos != std::string::npos && brace_pos < len) {
            ic_highlight(henv, static_cast<long>(brace_pos), 1L, "cjsh-operator");
        }
        highlight_heredocs();
        return;
    }

    const auto& comparison_ops = token_constants::comparison_operators();
    HighlightPathContext paths;

    (void)command_analysis::visit_command_ranges(
        sanitized_input,
        [&](size_t command_start, size_t command_end) {
            highlight_command_range(henv, input, sanitized_input, command_start, command_end,
                                    comparison_ops, paths);
            return true;
        },
        [&](size_t separator_start, const command_analysis::CommandSeparator& separator) {
            if ((separator.length > 0) && separator.is_operator) {
                ic_highlight(henv, static_cast<long>(separator_start),
                             static_cast<long>(separator.length), "cjsh-operator");
            }
        });

    size_t scan_start = 0;
    for (const auto& range : comment_ranges) {
        if (range.start > scan_start) {
            size_t segment_len = range.start - scan_start;
            highlight_quotes_and_variables(henv, input, scan_start, segment_len);
            highlight_compound_redirections(henv, input, scan_start, segment_len);
        }
        scan_start = range.end;
    }
    if (scan_start < len) {
        size_t segment_len = len - scan_start;
        highlight_quotes_and_variables(henv, input, scan_start, segment_len);
        highlight_compound_redirections(henv, input, scan_start, segment_len);
    }

    for (const auto& range : comment_ranges) {
        if (range.end > range.start) {
            ic_highlight(henv, static_cast<long>(range.start),
                         static_cast<long>(range.end - range.start), "cjsh-comment");
        }
    }

    highlight_heredocs();
}
