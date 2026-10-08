/*
  loop_evaluator.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_LOOP_EVALUATOR_H
#define CJSH_CORE_SRC_INTERPRETER_LOOP_EVALUATOR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Parser;

namespace loop_evaluator {

enum class LoopFlow : std::uint8_t {
    NONE,
    CONTINUE,
    BREAK
};

enum class LoopCondition : std::uint8_t {
    WHILE,
    UNTIL
};

struct LoopCommandOutcome {
    LoopFlow flow;
    int code;
};

LoopCommandOutcome handle_loop_command_result(int rc, bool allow_error_continue);

int handle_for_block(
    const std::vector<std::string>& src_lines, size_t& idx,
    const std::function<int(const std::vector<std::string>&)>& execute_block,
    const std::function<long long(const std::string&)>& evaluate_arithmetic_expression,
    const std::function<int(const std::string&)>& execute_simple_or_pipeline, Parser* parser,
    const std::function<bool()>& should_abort_execution = nullptr);

int handle_select_block(const std::vector<std::string>& src_lines, size_t& idx,
                        const std::function<int(const std::vector<std::string>&)>& execute_block,
                        const std::function<int(const std::string&)>& execute_simple_or_pipeline,
                        Parser* parser,
                        const std::function<bool()>& should_abort_execution = nullptr);

int handle_condition_loop_block(
    LoopCondition condition, const std::vector<std::string>& src_lines, size_t& idx,
    const std::function<int(const std::vector<std::string>&)>& execute_block,
    const std::function<int(const std::string&)>& execute_simple_or_pipeline, Parser* parser,
    const std::function<bool()>& should_abort_execution = nullptr);

std::optional<int> try_execute_inline_do_block(
    const std::string& first_segment, const std::vector<std::string>& segments,
    size_t& segment_index,
    const std::function<int(const std::vector<std::string>&, size_t&)>& handler);

}  // namespace loop_evaluator

#endif  // CJSH_CORE_SRC_INTERPRETER_LOOP_EVALUATOR_H
