/*
  case_evaluator.cpp

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

#include "case_evaluator.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "interpreter_utils.h"
#include "parser.h"
#include "parser_utils.h"
#include "shell_env.h"

using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;

namespace case_evaluator {

std::pair<std::string, size_t> collect_case_body(const std::vector<std::string>& src_lines,
                                                 size_t start_index) {
    std::ostringstream body_stream;
    bool appended = false;
    size_t end_index = start_index;
    for (size_t i = start_index; i < src_lines.size(); ++i) {
        std::string raw = strip_inline_comment(src_lines[i]);
        std::string trimmed_line = trim(raw);
        if (trimmed_line.empty()) {
            continue;
        }
        size_t esac_pos = parser_find_keyword_token(trimmed_line, "esac");
        if (esac_pos != std::string::npos) {
            std::string before_esac = trim(trimmed_line.substr(0, esac_pos));
            if (!before_esac.empty()) {
                if (appended) {
                    body_stream << '\n';
                }
                body_stream << before_esac;
            }
            end_index = i;
            return {body_stream.str(), end_index};
        }
        if (appended) {
            body_stream << '\n';
        }
        body_stream << trimmed_line;
        appended = true;
    }
    return {body_stream.str(), src_lines.size()};
}

std::vector<std::string> split_case_sections(const std::string& input, bool trim_sections) {
    std::vector<std::string> sections;
    sections.reserve(4);
    size_t start = 0;
    while (start < input.length()) {
        size_t sep_pos = std::string::npos;
        size_t terminator_length = 0;
        char quote = '\0';
        bool escaped = false;
        for (size_t i = start; i < input.size(); ++i) {
            const char ch = input[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                escaped = true;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                if (quote == '\0') {
                    quote = ch;
                } else if (quote == ch) {
                    quote = '\0';
                }
                continue;
            }
            if (quote != '\0') {
                continue;
            }
            if (input.compare(i, 3, ";;&") == 0) {
                sep_pos = i;
                terminator_length = 3;
                break;
            }
            if (input.compare(i, 2, ";;") == 0 || input.compare(i, 2, ";&") == 0) {
                sep_pos = i;
                terminator_length = 2;
                break;
            }
        }
        std::string section;
        if (sep_pos == std::string::npos) {
            (void)section.assign(input, start, std::string::npos);
            start = input.length();
        } else {
            (void)section.assign(input, start, sep_pos - start + terminator_length);
            start = sep_pos + terminator_length;
        }
        if (trim_sections) {
            section = trim(section);
        }
        sections.push_back(std::move(section));
    }
    return sections;
}

std::string normalize_case_pattern(std::string pattern, Parser* parser) {
    if (parser != nullptr) {
        parser->expand_env_vars(pattern);
    }
    return pattern;
}

std::string normalize_case_value(std::string value, Parser* parser) {
    if (value.length() >= 2) {
        char first_char = value.front();
        char last_char = value.back();
        if ((first_char == '"' && last_char == '"') || (first_char == '\'' && last_char == '\'')) {
            value = value.substr(1, value.length() - 2);
        }
    }

    (void)strip_subst_literal_markers(value);

    if (!value.empty() && parser != nullptr) {
        parser->expand_env_vars_selective(value);
    }

    auto stripped = strip_noenv_sentinels(value);
    value = std::move(stripped.first);

    return value;
}

bool parse_case_section(const std::string& section, CaseSectionData& out, Parser* parser) {
    size_t paren_pos = std::string::npos;
    int pattern_group_depth = 0;
    bool escaped = false;
    for (size_t i = 0; i < section.size(); ++i) {
        char ch = section[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '(' &&
            (pattern_group_depth > 0 ||
             (i > 0 && (section[i - 1] == '?' || section[i - 1] == '*' || section[i - 1] == '+' ||
                        section[i - 1] == '@' || section[i - 1] == '!')))) {
            ++pattern_group_depth;
            continue;
        }
        if (ch == ')' && pattern_group_depth > 0) {
            --pattern_group_depth;
            continue;
        }
        if (ch == ')') {
            paren_pos = i;
            break;
        }
    }
    if (paren_pos == std::string::npos) {
        return false;
    }
    out.raw_pattern = trim(section.substr(0, paren_pos));
    if (!out.raw_pattern.empty() && out.raw_pattern.front() == '(') {
        (void)out.raw_pattern.erase(0, 1);
        out.raw_pattern = trim(out.raw_pattern);
    }
    out.command = trim(section.substr(paren_pos + 1));
    if (out.command.length() >= 3 && out.command.substr(out.command.length() - 3) == ";;&") {
        out.terminator = CaseTerminator::ContinueMatching;
        out.command = trim(out.command.substr(0, out.command.length() - 3));
    } else if (out.command.length() >= 2 && out.command.substr(out.command.length() - 2) == ";&") {
        out.terminator = CaseTerminator::FallThrough;
        out.command = trim(out.command.substr(0, out.command.length() - 2));
    } else if (out.command.length() >= 2 && out.command.substr(out.command.length() - 2) == ";;") {
        out.terminator = CaseTerminator::Break;
        out.command = trim(out.command.substr(0, out.command.length() - 2));
    } else {
        out.terminator = CaseTerminator::End;
    }
    out.pattern = normalize_case_pattern(out.raw_pattern, parser);
    return true;
}

bool execute_case_sections(
    const std::vector<std::string>& sections, const std::string& case_value,
    const std::function<int(const std::string&)>& executor, int& matched_exit_code, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher) {
    matched_exit_code = 0;
    std::vector<std::string> filtered_sections;
    filtered_sections.reserve(sections.size());
    for (const auto& raw_section : sections) {
        std::string trimmed_section = trim(raw_section);
        if (!trimmed_section.empty()) {
            filtered_sections.push_back(trimmed_section);
        }
    }

    bool matched_any = false;
    bool execute_unconditionally = false;

    for (const auto& section : filtered_sections) {
        CaseSectionData data;
        if (!parse_case_section(section, data, parser)) {
            continue;
        }

        bool pattern_matches = execute_unconditionally || pattern_matcher(case_value, data.pattern);
        if (!pattern_matches) {
            continue;
        }

        matched_any = true;

        if (!data.command.empty()) {
            if (parser != nullptr) {
                auto semicolon_commands = parser->parse_semicolon_commands(data.command, true);
                for (const auto& subcmd : semicolon_commands) {
                    matched_exit_code = executor(subcmd);
                    if (matched_exit_code != 0 ||
                        shell_script_interpreter::detail::control_flow_pending() ||
                        cjsh_env::exit_requested()) {
                        break;
                    }
                }
            } else {
                matched_exit_code = executor(data.command);
            }
        }

        if (shell_script_interpreter::detail::control_flow_pending() ||
            cjsh_env::exit_requested()) {
            return true;
        }

        switch (data.terminator) {
            case CaseTerminator::FallThrough:
                execute_unconditionally = true;
                break;
            case CaseTerminator::ContinueMatching:
                execute_unconditionally = false;
                break;
            case CaseTerminator::Break:
            case CaseTerminator::End:
                return true;
        }
    }

    return matched_any;
}

std::string sanitize_case_patterns(const std::string& patterns) {
    size_t esac_pos = parser_find_keyword_token(patterns, "esac");
    if (esac_pos != std::string::npos) {
        return patterns.substr(0, esac_pos);
    }
    return patterns;
}

std::pair<bool, int> evaluate_case_patterns(
    const std::string& patterns, const std::string& case_value, bool trim_sections,
    const std::function<int(const std::string&)>& executor, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher) {
    auto sanitized = sanitize_case_patterns(patterns);
    auto sections = split_case_sections(sanitized, trim_sections);
    int matched_exit_code = 0;
    bool matched = execute_case_sections(sections, case_value, executor, matched_exit_code, parser,
                                         pattern_matcher);
    return {matched, matched_exit_code};
}

std::optional<int> handle_inline_case(
    const std::string& text, const std::function<int(const std::string&)>& executor,
    bool allow_command_substitution, bool trim_sections, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher,
    const std::function<std::pair<std::string, std::vector<std::string>>(const std::string&)>&
        command_substitution_expander) {
    if (text != "case" && text.rfind("case ", 0) != 0) {
        return std::nullopt;
    }
    if (text.find(" in ") == std::string::npos || text.find("esac") == std::string::npos) {
        return std::nullopt;
    }

    size_t in_pos = text.find(" in ");
    std::string case_part = text.substr(0, in_pos);
    std::string patterns_part = text.substr(in_pos + 4);
    std::string processed_case_part = case_part;

    if (allow_command_substitution && processed_case_part.find("$(") != std::string::npos) {
        auto expansion = command_substitution_expander(processed_case_part);
        processed_case_part = expansion.first;
    }

    std::string case_value;
    std::string raw_case_value;

    auto extract_case_value = [&] {
        size_t space_pos = processed_case_part.find(' ');
        if (space_pos != std::string::npos && processed_case_part.substr(0, space_pos) == "case") {
            return trim(processed_case_part.substr(space_pos + 1));
        }
        return std::string{};
    };

    raw_case_value = extract_case_value();

    if (raw_case_value.empty() && parser != nullptr) {
        std::vector<std::string> case_tokens = parser->parse_command(processed_case_part);
        if (case_tokens.size() >= 2 && case_tokens[0] == "case") {
            raw_case_value = case_tokens[1];
        }
    }

    case_value = normalize_case_value(raw_case_value, parser);

    auto case_result = evaluate_case_patterns(patterns_part, case_value, trim_sections, executor,
                                              parser, pattern_matcher);
    return case_result.first ? std::optional<int>{case_result.second} : std::optional<int>{0};
}

}  // namespace case_evaluator
