/*
  builtin.h

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

#ifndef CJSH_CORE_SRC_CORE_BUILTIN_H
#define CJSH_CORE_SRC_CORE_BUILTIN_H

#include <limits.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class Shell;

bool is_posix_special_builtin(const std::string& name);
int posix_special_builtin_error(int status);

// `command` removes special-builtin error behavior from its immediate operand.
class RegularBuiltinScope {
   public:
    explicit RegularBuiltinScope(std::string name);
    ~RegularBuiltinScope();
    RegularBuiltinScope(const RegularBuiltinScope&) = delete;
    RegularBuiltinScope& operator=(const RegularBuiltinScope&) = delete;

   private:
    std::string previous;
};

class Built_ins {
   public:
    Built_ins();
    ~Built_ins();

    void set_shell(Shell* shell_ptr);
    std::string get_current_directory() const;
    std::string get_previous_directory() const;
    void set_current_directory();

    int builtin_command(const std::vector<std::string>& args);
    int is_builtin_command(const std::string& cmd) const;
    int builtin_or_runtime_command(const std::vector<std::string>& args);
    int is_builtin_or_runtime_command(const std::string& cmd) const;

    std::vector<std::string> get_builtin_commands() const;

   private:
    std::string current_directory;
    std::string previous_directory;
    std::unordered_map<std::string, std::function<int(const std::vector<std::string>&)>> builtins;
    Shell* shell;
};

#endif  // CJSH_CORE_SRC_CORE_BUILTIN_H
