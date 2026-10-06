/*
  conditional_evaluator.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_CONDITIONAL_EVALUATOR_H
#define CJSH_CORE_SRC_INTERPRETER_CONDITIONAL_EVALUATOR_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class Parser;

namespace conditional_evaluator {

int handle_if_block(const std::vector<std::string>& src_lines, size_t& idx,
                    const std::function<int(const std::vector<std::string>&)>& execute_block,
                    const std::function<int(const std::string&)>& execute_simple_or_pipeline,
                    const std::function<int(const std::string&)>& evaluate_logical_condition,
                    Parser* shell_parser,
                    const std::function<bool()>& should_abort_execution = nullptr);

std::string simplify_parentheses_in_condition(
    const std::string& condition, const std::function<int(const std::string&)>& evaluator);

int evaluate_logical_condition(const std::string& condition,
                               const std::function<int(const std::string&)>& executor);

}  // namespace conditional_evaluator

#endif  // CJSH_CORE_SRC_INTERPRETER_CONDITIONAL_EVALUATOR_H
