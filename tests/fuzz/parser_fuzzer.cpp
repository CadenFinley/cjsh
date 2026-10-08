/*
  parser_fuzzer.cpp

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
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "parser.h"
#include "shell.h"
#include "tokenizer.h"

std::unique_ptr<Shell> shell;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }

    Parser parser;
    const std::string input =
        size == 0 ? std::string{} : std::string(reinterpret_cast<const char*>(data), size);
    // Lexical operations only: never expand words or execute fuzzed shell text.
    (void)parser.parse_logical_commands(input);
    (void)parser.parse_semicolon_commands(input, true);
    (void)parser.parse_into_lines(input);
    try {
        (void)Tokenizer::tokenize_command(input);
    } catch (const std::runtime_error&) {
        // An unfinished quote is an ordinary lexical rejection.
    }

    // Generate valid nested arithmetic too. Sanitizers alone cannot catch a
    // logical operator silently swallowed by inconsistent delimiter accounting.
    const std::size_t depth = size == 0 ? 0 : data[0] % 32;
    const std::string first =
        ": $((" + std::string(depth, '(') + "1 || 0" + std::string(depth, ')') + ")) ";
    const auto logical = parser.parse_logical_commands(first + "&& : yes || : no");
    if (logical.size() != 3 || logical[0].command != first || logical[0].op != "&&" ||
        logical[1].command != " : yes " || logical[1].op != "||" || logical[2].command != " : no" ||
        !logical[2].op.empty()) {
        std::abort();
    }
    return 0;
}
