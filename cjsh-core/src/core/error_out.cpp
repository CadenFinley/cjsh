/*
  error_out.cpp

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

#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "error_out.h"
#include "shell.h"
#include "shell_env.h"

namespace {

constexpr const char* kResetColor = "\x1b[0m";
constexpr const char* kErrorLogEnvVar = "CJSH_ERROR_LOG";

std::string expand_log_path(const char* raw_path) {
    if (!raw_path || raw_path[0] == '\0') {
        return {};
    }

    std::string path(raw_path);
    if (path[0] != '~') {
        return path;
    }

    if (path.size() > 1 && path[1] != '/') {
        return path;
    }

    std::string home = cjsh_env::get_shell_variable_value("HOME");
    if (home.empty()) {
        return path;
    }

    return home + path.substr(1);
}

void append_error_header(std::ostream& out, const ErrorInfo& error) {
    std::string header = cjsh_env::get_shell_variable_value("0");
    if (header.empty()) {
        header = "cjsh";
    } else {
        header = std::filesystem::path(header).filename().string();
        if (header.empty()) {
            header = "cjsh";
        }
    }
    if (config::login_mode && (header.empty() || header.front() != '-')) {
        (void)header.insert(header.begin(), '-');
    }
    out << header << ": ";

    if (!error.command_used.empty()) {
        out << error.command_used;
        if (error.type != ErrorType::UNKNOWN_ERROR) {
            out << ": ";
        }
    }

    switch (error.type) {
        case ErrorType::COMMAND_NOT_FOUND:
            out << "command not found";
            break;
        case ErrorType::SYNTAX_ERROR:
            out << "syntax error";
            break;
        case ErrorType::PERMISSION_DENIED:
            out << "permission denied";
            break;
        case ErrorType::FILE_NOT_FOUND:
            out << "file not found";
            break;
        case ErrorType::INVALID_ARGUMENT:
            out << "invalid argument";
            break;
        case ErrorType::RUNTIME_ERROR:
            out << "runtime error";
            break;
        case ErrorType::FATAL_ERROR:
            out << "fatal error";
            break;
        case ErrorType::UNKNOWN_ERROR:
        default:
            break;
    }

    if (!error.message.empty()) {
        out << ": " << error.message;
    }
}

void append_fatal_error_context(std::ostream& out, const ErrorInfo& error) {
    if (ErrorType::FATAL_ERROR != error.type) {
        return;
    }

    out << "(fatal error, cjsh will exit)\n";
}

void append_error_suggestions(std::ostream& out, const ErrorInfo& error) {
    if (!config::error_suggestions_enabled || error.suggestions.empty()) {
        return;
    }

    std::vector<std::string> commands;
    bool has_command_suggestions = false;

    for (const auto& suggestion : error.suggestions) {
        if (suggestion.find("Did you mean '") != std::string::npos) {
            size_t start = suggestion.find('\'') + 1;
            size_t end = suggestion.find('\'', start);
            if (start != std::string::npos && end != std::string::npos && end > start) {
                commands.push_back(suggestion.substr(start, end - start));
                has_command_suggestions = true;
            }
        }
    }

    if (has_command_suggestions && !commands.empty()) {
        out << "Did you mean: ";
        for (size_t i = 0; i < commands.size(); ++i) {
            out << commands[i];
            if (i < commands.size() - 1) {
                out << ", ";
            }
        }
        out << "?" << '\n';

        for (const auto& suggestion : error.suggestions) {
            if (suggestion.find("Did you mean '") == std::string::npos) {
                out << suggestion << '\n';
            }
        }
    } else {
        for (const auto& suggestion : error.suggestions) {
            out << suggestion << '\n';
        }
    }
}

std::string build_error_log_message(const ErrorInfo& error) {
    std::ostringstream out;
    append_error_header(out, error);
    out << '\n';
    append_fatal_error_context(out, error);
    append_error_suggestions(out, error);
    return out.str();
}

void append_error_log(const ErrorInfo& error) {
    std::string log_env = cjsh_env::get_shell_variable_value(kErrorLogEnvVar);
    std::string log_path = expand_log_path(log_env.c_str());
    if (log_path.empty()) {
        return;
    }

    std::filesystem::path path(log_path);
    std::error_code ec;
    std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        (void)std::filesystem::create_directories(parent, ec);
    }

    std::ofstream log_file(path, std::ios::app);
    if (!log_file.is_open()) {
        return;
    }

    log_file << build_error_log_message(error);
}

const char* severity_to_color(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO:
            return "\x1b[4m\x1b[58;5;51m";
        case ErrorSeverity::WARNING:
            return "\x1b[4m\x1b[58;5;214m";
        case ErrorSeverity::ERROR:
            return "\x1b[4m\x1b[58;5;160m";
        case ErrorSeverity::CRITICAL:
            return "\x1b[4m\x1b[58;5;196m";
        default:
            return "\x1b[4m\x1b[58;5;160m";
    }
}

bool should_colorize_output() {
    if (!config::colors_enabled) {
        return false;
    }

    if (!shell || !shell->get_interactive_mode()) {
        return false;
    }

    return isatty(STDERR_FILENO) != 0;
}

}  // namespace

ErrorInfo::ErrorInfo() : type(ErrorType::UNKNOWN_ERROR), severity(ErrorSeverity::ERROR) {
}

ErrorInfo::ErrorInfo(ErrorType t, ErrorSeverity s, const std::string& cmd, const std::string& msg,
                     const std::vector<std::string>& sugg)
    : type(t), severity(s), command_used(cmd), message(msg), suggestions(sugg) {
}

ErrorInfo::ErrorInfo(ErrorType t, const std::string& cmd, const std::string& msg,
                     const std::vector<std::string>& sugg)
    : type(t),
      severity(get_default_severity(t)),
      command_used(cmd),
      message(msg),
      suggestions(sugg) {
}

ErrorSeverity ErrorInfo::get_default_severity(ErrorType type) {
    switch (type) {
        case ErrorType::SYNTAX_ERROR:
            return ErrorSeverity::CRITICAL;
        case ErrorType::COMMAND_NOT_FOUND:
            return ErrorSeverity::ERROR;
        case ErrorType::PERMISSION_DENIED:
            return ErrorSeverity::ERROR;
        case ErrorType::FILE_NOT_FOUND:
            return ErrorSeverity::ERROR;
        case ErrorType::INVALID_ARGUMENT:
            return ErrorSeverity::INFO;
        case ErrorType::RUNTIME_ERROR:
            return ErrorSeverity::ERROR;
        case ErrorType::FATAL_ERROR:
            return ErrorSeverity::CRITICAL;
        case ErrorType::UNKNOWN_ERROR:
        default:
            return ErrorSeverity::ERROR;
    }
}

void print_error(const ErrorInfo& error) {
    append_error_log(error);
    const bool colorize_output = should_colorize_output();
    const char* color_prefix = colorize_output ? severity_to_color(error.severity) : "";
    const char* color_reset = colorize_output ? kResetColor : "";

    if (colorize_output) {
        std::cerr << color_prefix;
    }

    append_error_header(std::cerr, error);
    std::cerr << '\n';
    append_error_suggestions(std::cerr, error);

    if (colorize_output) {
        std::cerr << color_reset;
    }

    if (ErrorType::FATAL_ERROR == error.type) {
        // fatal errors exit
        _exit(EXIT_FAILURE);
    }
}

void print_error_errno(const ErrorInfo& error) {
    int saved_errno = errno;
    if (saved_errno == 0) {
        print_error(error);
        return;
    }

    ErrorInfo logged_error = error;
    std::string errno_message = std::strerror(saved_errno);
    if (!logged_error.message.empty()) {
        logged_error.message += ": ";
    }
    logged_error.message += errno_message;
    append_error_log(logged_error);

    const bool colorize_output = should_colorize_output();
    const char* color_prefix = colorize_output ? severity_to_color(error.severity) : "";
    const char* color_reset = colorize_output ? kResetColor : "";

    if (colorize_output) {
        std::cerr << color_prefix;
    }

    std::ostringstream prefix;
    append_error_header(prefix, error);
    errno = saved_errno;
    std::perror(prefix.str().c_str());
    append_error_suggestions(std::cerr, error);

    if (colorize_output) {
        std::cerr << color_reset;
    }

    if (ErrorType::FATAL_ERROR == error.type) {
        // fatal errors exit
        _exit(EXIT_FAILURE);
    }
}
