/*
  parameter_expansion_evaluator.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_PARAMETER_EXPANSION_EVALUATOR_H
#define CJSH_CORE_SRC_INTERPRETER_PARAMETER_EXPANSION_EVALUATOR_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ParameterExpansionEvaluator {
   public:
    using VariableReader = std::function<std::string(const std::string&)>;
    using VariableWriter = std::function<void(const std::string&, const std::string&)>;
    using VariableChecker = std::function<bool(const std::string&)>;
    using PatternMatcher = std::function<bool(const std::string&, const std::string&)>;
    using ArrayLengthReader = std::function<std::optional<size_t>(const std::string&)>;
    using ArrayKeysReader = std::function<std::string(const std::string&)>;
    using WordExpander = std::function<std::string(const std::string&)>;
    using IndirectReader = std::function<std::string(const std::string&)>;
    using PatternEndpoints = std::function<std::optional<std::vector<size_t>>(
        const std::string&, const std::string&, bool)>;

    ParameterExpansionEvaluator(VariableReader var_reader, VariableWriter var_writer,
                                VariableChecker var_checker, PatternMatcher pattern_matcher,
                                ArrayLengthReader array_length_reader = nullptr,
                                ArrayKeysReader array_keys_reader = nullptr,
                                WordExpander word_expander = nullptr,
                                IndirectReader indirect_reader = nullptr,
                                PatternEndpoints pattern_endpoints = nullptr,
                                WordExpander pattern_word_expander = nullptr);
    std::string expand(const std::string& param_expr);

   private:
    VariableReader read_variable;
    VariableWriter write_variable;
    VariableChecker is_variable_set;
    PatternMatcher matches_pattern;
    ArrayLengthReader read_array_length;
    ArrayKeysReader read_array_keys;
    WordExpander expand_word;
    WordExpander expand_pattern_word;
    IndirectReader read_indirect;
    PatternEndpoints find_pattern_endpoints;

    std::string pattern_match_prefix(const std::string& value, const std::string& pattern,
                                     bool longest);
    std::string pattern_match_suffix(const std::string& value, const std::string& pattern,
                                     bool longest);
    std::string pattern_substitute(const std::string& value, const std::string& replacement_expr,
                                   bool global);
    std::string case_convert(const std::string& value, const std::string& pattern, bool uppercase,
                             bool all_chars);
    bool try_evaluate_substring(const std::string& param_expr, std::string& result);
};

#endif  // CJSH_CORE_SRC_INTERPRETER_PARAMETER_EXPANSION_EVALUATOR_H
