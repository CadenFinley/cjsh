/*
  shopt_command.cpp

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

#include "shopt_command.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

#include "shell.h"
#include "shell_env.h"

int shopt_command(const std::vector<std::string>& args, Shell* shell) {
    if (!shell) {
        return 1;
    }
    bool reusable = false, quiet = false, set_options = false;
    std::optional<bool> change;
    size_t first = 1;
    for (; first < args.size(); ++first) {
        const auto& arg = args[first];
        if (arg == "--") {
            ++first;
            break;
        }
        if (arg.size() < 2 || arg.front() != '-') {
            break;
        }
        for (size_t i = 1; i < arg.size(); ++i) {
            switch (arg[i]) {
                case 'p':
                    reusable = true;
                    break;
                case 'q':
                    quiet = true;
                    break;
                case 'o':
                    set_options = true;
                    break;
                case 's':
                case 'u':
                    if (change && *change != (arg[i] == 's')) {
                        std::cerr
                            << "cjsh: shopt: cannot set and unset shell options simultaneously\n";
                        return 1;
                    }
                    change = arg[i] == 's';
                    break;
                default:
                    std::cerr << "cjsh: shopt: -" << arg[i] << ": invalid option\n";
                    return 2;
            }
        }
    }

    std::vector<const ShellOptionDescriptor*> selected;
    int status = 0;
    if (first == args.size()) {
        for (const auto& descriptor : get_shell_option_descriptors()) {
            if (descriptor.shopt == !set_options &&
                (!change || shell->get_shell_option(descriptor.option) == *change)) {
                selected.push_back(&descriptor);
            }
        }
        std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
            return std::string_view(a->name) < std::string_view(b->name);
        });
    } else {
        for (size_t i = first; i < args.size(); ++i) {
            const auto& descriptors = get_shell_option_descriptors();
            const auto found = std::find_if(
                descriptors.begin(), descriptors.end(),
                [&](const auto& d) { return d.shopt == !set_options && args[i] == d.name; });
            if (found == descriptors.end()) {
                std::cerr << "cjsh: shopt: " << args[i] << ": invalid shell option name\n";
                status = 1;
            } else {
                selected.push_back(&*found);
            }
        }
    }
    for (const auto* descriptor : selected) {
        if (change && first < args.size()) {
            shell->set_shell_option(descriptor->option, *change);
            continue;
        }
        const bool enabled = shell->get_shell_option(descriptor->option);
        if (first < args.size() && !enabled) {
            status = 1;
        }
        if (quiet) {
            continue;
        }
        if (reusable) {
            std::cout << (set_options ? "set " : "shopt ")
                      << (set_options ? (enabled ? "-o " : "+o ") : (enabled ? "-s " : "-u "))
                      << descriptor->name << '\n';
        } else {
            std::cout << std::left << std::setw(20) << descriptor->name << '\t'
                      << (enabled ? "on" : "off") << '\n';
        }
    }
    return status;
}
