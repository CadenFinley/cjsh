/*
  cjshopt_command.cpp

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

#include "cjshopt_command.h"
#include "cjshopt_registry.h"

#include "builtin_help.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "error_out.h"
#include "shell_env.h"

namespace {

const std::vector<std::string>& cjshopt_usage_lines() {
    static const std::vector<std::string> usage = [] {
        std::vector<std::string> lines = {
            "Usage: cjshopt <subcommand> [options]", "",
            "Configure shell behavior and interactive editing.",
            "Use 'cjshopt <subcommand> --help' for details and examples."};
        std::vector<const CjshoptSubcommandDescriptor*> commands;
        for (const auto& command : cjshopt_subcommands()) {
            commands.push_back(&command);
        }
        std::sort(commands.begin(), commands.end(), [](const auto* left, const auto* right) {
            return left->help_order < right->help_order;
        });
        std::string section;
        for (const auto* command : commands) {
            if (section != command->section) {
                section = command->section;
                lines.emplace_back();
                lines.push_back(section);
            }
            for (const auto& form : command->usage) {
                lines.push_back("  " + std::string(command->name) + " " + form.arguments);
                for (const char* description : form.description) {
                    lines.push_back("    " + std::string(description));
                }
            }
        }
        lines.insert(
            lines.end(),
            {"", "Add settings to your startup files (usually ~/.cjshrc) to persist them.",
             "Use 'cjshopt keybind ext --help' for custom command keybindings.",
             "For startup file generators, --force overwrites and --alt uses ~/.config/cjsh."});
        return lines;
    }();
    return usage;
}

void print_cjshopt_usage() {
    for (const auto& line : cjshopt_usage_lines()) {
        std::cout << line << '\n';
    }
}

std::string available_subcommands_message() {
    std::string message = "Available subcommands: ";
    for (size_t i = 0; i < cjshopt_subcommands().size(); ++i) {
        if (i != 0) {
            message += ", ";
        }
        message += cjshopt_subcommands()[i].name;
    }
    return message;
}
}  // namespace

int cjshopt_command(const std::vector<std::string>& args) {
    if (builtin_handle_help_with_startup_guard(args, {}, BuiltinHelpScanMode::FirstArgument)) {
        if (!cjsh_env::startup_active()) {
            print_cjshopt_usage();
        }
        return 0;
    }

    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "cjshopt", "Missing subcommand argument",
                     cjshopt_usage_lines()});

        return 1;
    }

    const std::string& subcommand = args[1];
    const auto* descriptor = find_cjshopt_subcommand(subcommand);
    if (descriptor != nullptr) {
        return descriptor->handler(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    print_error({ErrorType::INVALID_ARGUMENT,
                 "cjshopt",
                 "unknown subcommand '" + subcommand + "'",
                 {available_subcommands_message()}});

    return 1;
}
