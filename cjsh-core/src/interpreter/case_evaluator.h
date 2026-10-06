/*
  case_evaluator.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_CASE_EVALUATOR_H
#define CJSH_CORE_SRC_INTERPRETER_CASE_EVALUATOR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Parser;

namespace case_evaluator {

enum class CaseTerminator : std::uint8_t {
    Break,
    FallThrough,
    ContinueMatching,
    End
};

struct CaseSectionData {
    std::string raw_pattern;
    std::string pattern;
    std::string command;
    CaseTerminator terminator = CaseTerminator::End;
};

std::pair<std::string, size_t> collect_case_body(const std::vector<std::string>& src_lines,
                                                 size_t start_index);

std::vector<std::string> split_case_sections(const std::string& input, bool trim_sections);

std::string normalize_case_pattern(std::string pattern, Parser* parser);

std::string normalize_case_value(std::string value, Parser* parser);

bool parse_case_section(const std::string& section, CaseSectionData& out, Parser* parser);

bool execute_case_sections(
    const std::vector<std::string>& sections, const std::string& case_value,
    const std::function<int(const std::string&)>& executor, int& matched_exit_code, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher);

std::string sanitize_case_patterns(const std::string& patterns);

std::pair<bool, int> evaluate_case_patterns(
    const std::string& patterns, const std::string& case_value, bool trim_sections,
    const std::function<int(const std::string&)>& executor, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher);

std::optional<int> handle_inline_case(
    const std::string& text, const std::function<int(const std::string&)>& executor,
    bool allow_command_substitution, bool trim_sections, Parser* parser,
    const std::function<bool(const std::string&, const std::string&)>& pattern_matcher,
    const std::function<std::pair<std::string, std::vector<std::string>>(const std::string&)>&
        command_substitution_expander);

}  // namespace case_evaluator

#endif  // CJSH_CORE_SRC_INTERPRETER_CASE_EVALUATOR_H
