/*
  pattern_matcher.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_PATTERN_MATCHER_H
#define CJSH_CORE_SRC_INTERPRETER_PATTERN_MATCHER_H

#include <optional>
#include <string>
#include <vector>

class PatternMatcher {
   public:
    PatternMatcher() = default;
    ~PatternMatcher() = default;

    PatternMatcher(const PatternMatcher&) = delete;
    PatternMatcher& operator=(const PatternMatcher&) = delete;
    PatternMatcher(PatternMatcher&&) = default;
    PatternMatcher& operator=(PatternMatcher&&) = default;

    bool matches_pattern(const std::string& text, const std::string& pattern,
                         bool top_level_alternatives = false) const;

    // One endpoint per starting byte, or string::npos when no substring matches.
    // Repeating/negated extended groups return nullopt for the general matcher;
    // ordinary globs and nested @() and ?() alternatives share suffix results.
    std::optional<std::vector<size_t>> match_end_positions(const std::string& text,
                                                           const std::string& pattern,
                                                           bool longest) const;
};

#endif  // CJSH_CORE_SRC_INTERPRETER_PATTERN_MATCHER_H
