/*
  syntax_validation_probe.cpp

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

#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include "interpreter.h"
#include "shell.h"
#include "shell_env.h"

std::unique_ptr<Shell> shell;

int main(int argc, char** argv) {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(false);
    config::interactive_mode = false;
    config::force_interactive = false;
    shell = std::make_unique<Shell>();
    shell->set_interactive_mode(false);
    auto* interpreter = shell->get_interpreter();
    for (int i = 1; i < argc; ++i) {
        auto lines = interpreter->parse_into_lines(argv[i]);
        std::cout << "INPUT " << std::quoted(argv[i]) << '\n';
        for (const auto& line : lines) {
            std::cout << "LINE " << std::quoted(line) << '\n';
        }
        std::cout << "MORE " << interpreter->needs_additional_input(lines) << '\n';
        for (const auto& error : interpreter->validate_comprehensive_syntax(lines, false, false)) {
            std::cout << "ERROR " << error.error_code << ' ' << error.position.line_number << ' '
                      << std::quoted(error.message) << '\n';
        }
    }
    shell.reset();
}
