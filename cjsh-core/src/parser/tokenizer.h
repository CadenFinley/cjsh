/*
  tokenizer.h

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

#ifndef CJSH_CORE_SRC_PARSER_TOKENIZER_H
#define CJSH_CORE_SRC_PARSER_TOKENIZER_H

#include <cstddef>
#include <string>
#include <vector>

class Shell;

class Tokenizer {
   public:
    // Combines redirections while retaining unquoted IO-number adjacency.
    static std::vector<std::string> tokenize_command(const std::string& cmdline);

    std::vector<std::string> split_by_ifs(const std::string& input, const std::string& ifs,
                                          const std::vector<bool>* expanded_bytes = nullptr);

   private:
    static std::vector<std::string> merge_redirection_tokens(
        const std::vector<std::string>& tokens, const std::vector<size_t>& io_number_tokens);
    static bool looks_like_assignment(const std::string& input);
};

#endif  // CJSH_CORE_SRC_PARSER_TOKENIZER_H
