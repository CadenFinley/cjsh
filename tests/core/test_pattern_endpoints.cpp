/*
  test_pattern_endpoints.cpp

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

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pattern_matcher.h"
#include "shell.h"
#include "shell_env.h"

std::unique_ptr<Shell> g_shell;

int main() {
    PatternMatcher matcher;
    // Compare every starting position against exhaustive full-pattern matching.
    // The reference exercises the existing general matcher, not the endpoint DP.
    const std::vector<std::string> patterns = {"",
                                               "a",
                                               "*",
                                               "?",
                                               "a*b",
                                               "[ab]*",
                                               "[!a]",
                                               "[[:alpha:]]",
                                               "@(a|b)",
                                               "@(a|ab)",
                                               "@(ab|a)b",
                                               "@(a*|b)a",
                                               "@(a|)b",
                                               "@(a|b)*",
                                               "*@(ab|b)",
                                               "?(a|b)",
                                               "?(ab|a)b",
                                               "a?(a|b)*b",
                                               "?()",
                                               "@()",
                                               "@(a|?(b))a",
                                               "?(@(a|ab)|b)?(a)",
                                               "@(a*|?b)*a",
                                               "@(a|b)@(a|b)",
                                               "@([ab]|[!b])",
                                               "@(\\*|a)",
                                               "'@(a|b)'",
                                               "a\\*b"};
    std::vector<std::string> texts = {"", "*", "a*b", "c", "aba c", "@(a|b)"};
    for (size_t length = 1; length <= 6; ++length) {
        for (size_t bits = 0; bits < (size_t{1} << length); ++bits) {
            std::string text(length, 'a');
            for (size_t pos = 0; pos < length; ++pos) {
                if ((bits & (size_t{1} << pos)) != 0) {
                    text[pos] = 'b';
                }
            }
            texts.push_back(std::move(text));
        }
    }

    size_t checks = 0;
    for (bool enabled : {false, true}) {
        config::extglob_enabled = enabled;
        for (const auto& pattern : patterns) {
            for (const auto& text : texts) {
                for (bool longest : {false, true}) {
                    const auto endpoints = matcher.match_end_positions(text, pattern, longest);
                    if (!endpoints || endpoints->size() != text.size() + 1) {
                        (void)std::fprintf(stderr, "missing endpoints: %s\n", pattern.c_str());
                        return 1;
                    }
                    for (size_t begin = 0; begin <= text.size(); ++begin) {
                        size_t expected = std::string::npos;
                        for (size_t end = begin; end <= text.size(); ++end) {
                            if (matcher.matches_pattern(text.substr(begin, end - begin), pattern)) {
                                expected = end;
                                if (!longest) {
                                    break;
                                }
                            }
                        }
                        ++checks;
                        if ((*endpoints)[begin] != expected) {
                            (void)std::fprintf(stderr,
                                               "endpoint mismatch: text=%s pattern=%s start=%zu "
                                               "longest=%d extglob=%d expected=%zu got=%zu\n",
                                               text.c_str(), pattern.c_str(), begin, longest,
                                               enabled, expected, (*endpoints)[begin]);
                            return 1;
                        }
                    }
                }
            }
        }
    }

    config::extglob_enabled = true;
    for (const auto& pattern : {"+(a|b)", "*(a)", "!(a)", "@(a|+(b))", "?(!(a))"}) {
        for (bool longest : {false, true}) {
            if (matcher.match_end_positions("aab", pattern, longest)) {
                (void)std::fprintf(stderr, "unsupported group did not fall back: %s\n", pattern);
                return 1;
            }
        }
    }
    // Toggling the parser option must invalidate the compiled pattern cache.
    config::extglob_enabled = false;
    const auto literal = matcher.match_end_positions("@(a|b)", "@(a|b)", true);
    if (!literal || (*literal)[0] != 6) {
        return 1;
    }
    (void)std::printf("PASS: %zu endpoint comparisons and fallback/option regressions\n", checks);
    return 0;
}
