/*
  flags.h

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

#ifndef CJSH_CORE_SRC_CORE_FLAGS_H
#define CJSH_CORE_SRC_CORE_FLAGS_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace flags {

struct ParseResult {
    std::string script_file;
    std::vector<std::string> script_args;
    std::vector<std::pair<std::string, bool>> shell_options;
    // (shopt namespace, reusable output) for invocation -o/+o/-O/+O without names.
    std::vector<std::pair<bool, bool>> option_queries;
    int exit_code = 0;
    bool should_exit = false;
};

ParseResult parse_arguments(int argc, char* argv[]);
bool is_login_shell_invocation();
void warn_if_invoked_via_sh();
void save_startup_arguments(int argc, char* argv[]);
std::vector<std::string>& startup_args();

void set_positional_parameters(const std::vector<std::string>& params);
int shift_positional_parameters(int count = 1);
std::vector<std::string> get_positional_parameters();
size_t get_positional_parameter_count();

}  // namespace flags

#endif  // CJSH_CORE_SRC_CORE_FLAGS_H
