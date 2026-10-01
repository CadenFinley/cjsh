/*
  usage.cpp

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

#include "usage.h"

#include <iostream>
#include <sstream>
#include <string>

#include "version_command.h"

std::string get_usage() {
    std::ostringstream usage_text;
    usage_text
        << "Usage: cjsh [options] [script_file [args...]]\n"
        << "       cjsh -c command_string [args...]\n"
        << "\n"
        << "Options:\n"
        << "      --help                 Display this help message and exit\n"
        << "      --version              Print version information and exit\n"
        << "  -l, --login                Start as a login shell (load ~/.cjprofile)\n"
        << "  -i, --interactive          Force interactive mode\n"
        << "  -c, --command COMMAND      Execute the specified command and exit\n"
        << "                             (disables history expansion)\n"
        << "  -n, --no-exec             Check syntax without execution or startup files\n"
        << "      --no-config           Skip all automatic startup/logout files\n"
        << "      --config-dir DIR      Native config root (overrides CJSH_CONFIG_HOME)\n"
        << "      --no-system-paths     Skip PATH setup from /etc/paths and /etc/paths.d\n"
        << "      --dialect NAME        Select cjsh or posix semantics\n"
        << "      --posix               Select POSIX.1-2024 mode\n"
        << "      -s stdin, -v verbose, -C noclobber, -m monitor, -h hashall\n"
        << "      -abBCefhHmnuvx / +abBCefhHmnuvx, -o / +o OPTION\n"
        << "      -O / +O OPTION        Enable/disable a shopt option\n"
        << "      Short options have the same meanings in every dialect.\n"
        << "\n"
        << "Feature Control Options:\n"
        << "      --minimal              Disable cjsh extras (colors, completions,\n"
        << "                             completion learning,\n"
        << "                             syntax highlighting, smart cd, rc sourcing,\n"
        << "                             title line, history expansion, status line,\n"
        << "                             multiline line numbers, startup time banner,\n"
        << "                             error suggestions, prompt vars, special handlers)\n"
        << "      --no-prompt-vars      Ignore PS1/PS2 and use fixed prompts\n"
        << "      --no-colors            Disable color output\n"
        << "  -N, --no-source            Don't source the ~/.cjshrc file\n"
        << "      --no-completions       Disable tab completions\n"
        << "      --no-completion-learning Disable on-demand completion learning\n"
        << "      --no-smart-cd          Disable smart cd auto-jumps\n"
        << "      --no-script-extension-interpreter Disable extension-based script runners\n"
        << "  -S, --no-syntax-highlighting Disable syntax highlighting\n"
        << "      --no-error-suggestions Disable error suggestions\n"
        << "      --no-agent            Disable agent assistance\n"
        << "      --no-history-expansion Disable history expansion (!commands)\n"
        << "      --no-history           Disable history recording (also disables history "
           "expansion)\n"
        << "  -W, --no-sh-warning       Suppress the sh invocation warning\n"
        << "\n"
        << "Display Options:\n"
        << "  -L, --no-titleline         Disable title line on startup\n"
        << "  -U, --show-startup-time    Display shell startup time\n"

        << "\n"
        << "Security Options:\n"
        << "      --secure               Secure mode: skip ~/.cjshenv, ~/.cjprofile, ~/.cjshrc,\n"
        << "                             and ~/.cjlogout entirely; disable history persistence\n"
        << "                             and smart cd; ignore special handlers\n"
        << "\n"
        << "Examples:\n"
        << "  cjsh                       Start interactive shell\n"
        << "  cjsh script.sh arg1 arg2   Run script with arguments\n"
        << "  cjsh -c 'echo hello'       Execute command and exit\n"
        << "  cjsh -l                    Start login shell\n"
        << "  cjsh --minimal             Start with minimal features\n"
        << "\n";
    return usage_text.str();
}

int print_usage(bool print_version, bool print_hook, bool print_footer) {
    if (print_version) {
        (void)version_command({});
    }
    if (print_hook) {
        std::cout << "POSIX shell scripting meets modern shell features\n";
    }
    std::cout << get_usage();
    if (print_footer) {
        std::cout << "For more information:\n"
                  << "  Documentation: https://cadenfinley.github.io/cjsh/\n"
                  << "  Repository:    https://github.com/CadenFinley/cjsh\n"
                  << "  Run 'help' inside cjsh for built-in command reference\n";
    }
    return 0;
}
