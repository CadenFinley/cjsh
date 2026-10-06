/*
  history_expansion.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_HISTORY_EXPANSION_H
#define CJSH_CORE_SRC_INTERPRETER_HISTORY_EXPANSION_H

#include <cstddef>
#include <string>
#include <vector>

class HistoryExpansion {
   public:
    struct ExpansionResult {
        std::string expanded_command;
        bool was_expanded = false;
        bool should_echo = true;
        std::string error_message;
        bool has_error = false;
    };

    static ExpansionResult expand(const std::string& command,
                                  const std::vector<std::string>& history_entries,
                                  bool history_includes_current_command = false);
    static bool contains_history_expansion(const std::string& command);
    static std::string get_history_file_path();
    static std::vector<std::string> read_history_entries();

   private:
    static bool expand_double_bang(const std::string& command, size_t& pos,
                                   const std::vector<std::string>& history, std::string& result,
                                   std::string& error);

    static bool expand_history_number(const std::string& command, size_t& pos,
                                      const std::vector<std::string>& history, std::string& result,
                                      std::string& error);

    static bool expand_history_search(const std::string& command, size_t& pos,
                                      const std::vector<std::string>& history, std::string& result,
                                      std::string& error);

    static bool expand_previous_command_designator(const std::string& command, size_t& pos,
                                                   const std::vector<std::string>& history,
                                                   std::string& result, std::string& error);

    static bool expand_quick_substitution(const std::string& command,
                                          const std::vector<std::string>& history,
                                          std::string& result, std::string& error);

    static bool expand_word_designator(const std::string& command, size_t& pos,
                                       const std::string& referenced_command, std::string& result,
                                       std::string& error);

    static std::vector<std::string> split_into_words(const std::string& command);
    static bool is_word_char(char c);
    static std::string get_word_from_command(const std::string& command, int word_index);
    static std::string get_words_range(const std::string& command, int start, int end);
};

#endif  // CJSH_CORE_SRC_INTERPRETER_HISTORY_EXPANSION_H
