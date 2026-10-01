/*
  command_preprocessor.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_COMMAND_PREPROCESSOR_H
#define CJSH_CORE_SRC_INTERPRETER_COMMAND_PREPROCESSOR_H

#include <cstdint>
#include <map>
#include <string>

class CommandPreprocessor {
   public:
    struct PreprocessedCommand {
        std::string processed_text;
        std::map<std::string, std::string> here_documents;
        bool has_subshells = false;
        bool needs_special_handling = false;
    };

    static PreprocessedCommand preprocess(const std::string& command);

    static std::string process_here_documents(const std::string& command,
                                              std::map<std::string, std::string>& here_docs,
                                              bool* incomplete = nullptr);

   private:
    static std::string process_subshells(const std::string& command);

    static std::uint32_t next_placeholder_id();
};

#endif  // CJSH_CORE_SRC_INTERPRETER_COMMAND_PREPROCESSOR_H
