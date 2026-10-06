/*
  trap_command.h

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

#ifndef CJSH_CORE_SRC_BUILTIN_TRAP_COMMAND_H
#define CJSH_CORE_SRC_BUILTIN_TRAP_COMMAND_H

#include <string>
#include <utility>
#include <vector>

class Shell;

void trap_manager_set_trap(int signal, const std::string& command);

void trap_manager_remove_trap(int signal);

void trap_manager_execute_trap(int signal);

std::vector<std::pair<int, std::string>> trap_manager_list_traps();

bool trap_manager_has_trap(int signal);

void trap_manager_initialize();

void trap_manager_set_shell(Shell* shell);

void trap_manager_execute_exit_trap();
void trap_manager_execute_debug_trap();

int signal_name_to_number(const std::string& signal_name);

std::string signal_number_to_name(int signal_number);

int trap_command(const std::vector<std::string>& args);

#endif  // CJSH_CORE_SRC_BUILTIN_TRAP_COMMAND_H
