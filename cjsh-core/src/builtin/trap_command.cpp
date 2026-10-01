/*
  trap_command.cpp

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

#include "trap_command.h"
#include <signal.h>

#include "builtin_help.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "error_out.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"

namespace {

struct TrapManagerState {
    std::unordered_map<int, std::string> traps;
    std::unordered_map<int, struct sigaction> original_actions;
    Shell* shell_ref = nullptr;
    bool exit_trap_executed = false;
    bool has_exit_trap = false;
    std::string exit_trap_command;
};

TrapManagerState& trap_manager_state() {
    static TrapManagerState* state = new TrapManagerState();
    return *state;
}

}  // namespace

void trap_manager_initialize() {
    (void)trap_manager_state();
}

void trap_manager_set_trap(int signal, const std::string& command) {
    if (signal == SIGKILL || signal == SIGSTOP) {
        return;
    }

    auto& state = trap_manager_state();

    if (signal > 0 && state.traps.find(signal) == state.traps.end()) {
        struct sigaction action{};
        if (sigaction(signal, nullptr, &action) == 0) {
            state.original_actions[signal] = action;
        }
    }
    state.traps[signal] = command;

    if (signal == 0) {
        state.has_exit_trap = true;
        state.exit_trap_command = command;
        return;
    }

    if (signal == -2 || signal == -3 || signal == -4) {
        return;
    }

    if (command.empty() || command == "-") {
        SignalHandler::ignore_signal(signal);
    } else {
        SignalHandler::set_signal_disposition(signal, SignalDisposition::TRAPPED);
    }
}

void trap_manager_remove_trap(int signal) {
    auto& state = trap_manager_state();
    (void)state.traps.erase(signal);
    auto original = state.original_actions.find(signal);
    if (original != state.original_actions.end()) {
        SignalHandler::restore_signal_disposition(signal, original->second);
        state.original_actions.erase(original);
    }

    if (signal == 0) {
        state.has_exit_trap = false;
        state.exit_trap_command.clear();
    }
}

void trap_manager_execute_trap(int signal) {
    auto& state = trap_manager_state();
    auto it = state.traps.find(signal);
    if (it != state.traps.end() && (state.shell_ref != nullptr)) {
        (void)state.shell_ref->execute(it->second);
    }
}

std::vector<std::pair<int, std::string>> trap_manager_list_traps() {
    auto& state = trap_manager_state();
    std::vector<std::pair<int, std::string>> result;
    result.reserve(state.traps.size());
    for (const auto& pair : state.traps) {
        result.push_back(pair);
    }
    return result;
}

bool trap_manager_has_trap(int signal) {
    auto& state = trap_manager_state();
    return state.traps.find(signal) != state.traps.end();
}

void trap_manager_set_shell(Shell* shell) {
    trap_manager_state().shell_ref = shell;
}

void trap_manager_execute_exit_trap() {
    auto& state = trap_manager_state();
    if (state.exit_trap_executed) {
        return;
    }
    state.exit_trap_executed = true;

    if (state.has_exit_trap && (state.shell_ref != nullptr)) {
        (void)state.shell_ref->execute(state.exit_trap_command);
    }
}

void trap_manager_execute_debug_trap() {
    auto& state = trap_manager_state();
    auto it = state.traps.find(-3);
    if (it != state.traps.end() && (state.shell_ref != nullptr)) {
        (void)state.shell_ref->execute(it->second);
    }
}

int signal_name_to_number(const std::string& signal_name) {
    return SignalHandler::parse_trap_signal_token(signal_name);
}

std::string signal_number_to_name(int signal_number) {
    switch (signal_number) {
        case 0:
            return "EXIT";
        case -2:
            return "ERR";
        case -3:
            return "DEBUG";
        case -4:
            return "RETURN";
        default:
            break;
    }

    return SignalHandler::signal_to_name(signal_number, true);
}

void print_trap_list(const std::vector<std::pair<int, std::string>>& traps) {
    for (const auto& pair : traps) {
        std::cout << "trap -- " << cjsh_env::quote_shell_value(pair.second) << " "
                  << signal_number_to_name(pair.first) << '\n';
    }
}

int trap_command(const std::vector<std::string>& args) {
    static const std::vector<std::string> help_lines = {
        "Usage: trap [ARG] [SIGNAL ...]", "       trap -l", "       trap -p",
        "Set a command to execute when SIGNAL is received.",
        "With no arguments or with -p, list active traps. -l lists available signals."};
    if (builtin_handle_help(args, help_lines)) {
        return 0;
    }
    if (args.size() == 1) {
        auto traps = trap_manager_list_traps();
        if (traps.empty()) {
            return 0;
        }
        print_trap_list(traps);
        return 0;
    }

    if (args.size() >= 2 && args[1] == "-l") {
        if (args.size() != 2) {
            print_error(
                {ErrorType::INVALID_ARGUMENT, "trap", "-l accepts no operands", help_lines});
            return 2;
        }
        for (const auto& pair : SignalHandler::trap_signal_names()) {
            std::cout << pair.first << ") SIG" << pair.second << '\n';
        }
        return 0;
    }

    size_t operand = 1;
    const bool print = args[operand] == "-p";
    if (print) {
        ++operand;
    }
    if (operand < args.size() && args[operand] == "--") {
        ++operand;
    } else if (!print && operand < args.size() && args[operand].size() > 1 &&
               args[operand][0] == '-') {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "trap", "invalid option: " + args[operand], help_lines});
        return 2;
    }

    const auto active = trap_manager_list_traps();
    auto print_condition = [&](int condition) {
        const auto it = std::find_if(active.begin(), active.end(), [condition](const auto& trap) {
            return trap.first == condition;
        });
        if (it != active.end()) {
            print_trap_list({*it});
        } else {
            std::cout << "trap -- - " << signal_number_to_name(condition) << '\n';
        }
    };
    if (print) {
        if (operand == args.size()) {
            if (config::is_posix_mode()) {
                print_condition(0);
                for (const auto& signal : SignalHandler::trap_signal_names()) {
                    print_condition(signal.first);
                }
            } else {
                print_trap_list(active);
            }
        } else {
            for (; operand < args.size(); ++operand) {
                const int condition = signal_name_to_number(args[operand]);
                if (condition == -1) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 "trap",
                                 args[operand] + ": invalid signal specification",
                                 {}});
                    return 1;
                }
                print_condition(condition);
            }
        }
        return 0;
    }
    if (operand == args.size()) {
        print_trap_list(active);
        return 0;
    }
    const bool numeric_reset =
        !args[operand].empty() && std::all_of(args[operand].begin(), args[operand].end(),
                                              [](unsigned char c) { return std::isdigit(c) != 0; });
    const std::string command = numeric_reset ? "-" : args[operand++];
    if (operand == args.size()) {
        print_error({ErrorType::INVALID_ARGUMENT, "trap", "missing signal operand", help_lines});
        return 2;
    }

    for (size_t i = operand; i < args.size(); ++i) {
        int signal_num = signal_name_to_number(args[i]);
        if (signal_num == -1) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "trap",
                         args[i] + ": invalid signal specification",
                         {}});
            return 1;
        }

        if (command == "-") {
            trap_manager_remove_trap(signal_num);
        } else {
            trap_manager_set_trap(signal_num, command);
        }
    }

    return 0;
}
