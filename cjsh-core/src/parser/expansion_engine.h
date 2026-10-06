/*
  expansion_engine.h

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

#ifndef CJSH_CORE_SRC_PARSER_EXPANSION_ENGINE_H
#define CJSH_CORE_SRC_PARSER_EXPANSION_ENGINE_H

#include <cstddef>
#include <string>
#include <vector>

class Shell;

class ExpansionEngine {
   public:
    explicit ExpansionEngine(Shell* shell = nullptr);

    std::vector<std::string> expand_braces(const std::string& pattern);
    std::vector<std::string> expand_wildcards(const std::string& pattern);

   private:
    Shell* shell;
    static constexpr size_t MAX_EXPANSION_SIZE = 10000000;

    void expand_and_append_results(const std::string& combined, std::vector<std::string>& result);

    template <typename T>
    void expand_range(T start, T end, const std::string& prefix, const std::string& suffix,
                      std::vector<std::string>& result, T stride = 1, size_t numeric_width = 0);
};

#endif  // CJSH_CORE_SRC_PARSER_EXPANSION_ENGINE_H
