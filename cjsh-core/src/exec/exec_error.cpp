/*
  exec_error.cpp

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

#include "error_out.h"
#include "exec.h"

#include <mutex>
#include <string>
#include <vector>

void Exec::set_error(const ErrorInfo& error) {
    std::lock_guard<std::mutex> lock(error_mutex);
    last_error = error;
}

void Exec::set_error(ErrorType type, const std::string& command, const std::string& message,
                     const std::vector<std::string>& suggestions) {
    ErrorInfo error = {type, command, message, suggestions};
    set_error(error);
}

ErrorInfo Exec::get_error() {
    std::lock_guard<std::mutex> lock(error_mutex);
    return last_error;
}

void Exec::print_last_error() {
    std::lock_guard<std::mutex> lock(error_mutex);
    print_error(last_error);
}

void Exec::print_error_if_needed(int exit_code) {
    if (exit_code == 0) {
        return;
    }

    ErrorInfo error = get_error();
    bool already_reported = (error.type == ErrorType::COMMAND_NOT_FOUND && error.message.empty());
    if (!already_reported && error.type == ErrorType::UNKNOWN_ERROR && error.command_used.empty() &&
        error.message.empty() && error.suggestions.empty()) {
        already_reported = true;
    }
    if (!already_reported && exit_code == 126 && error.type == ErrorType::PERMISSION_DENIED &&
        error.message.find("command failed with exit code") != std::string::npos) {
        already_reported = true;
    }
    const bool generic_runtime_exit_message =
        (error.type == ErrorType::RUNTIME_ERROR) &&
        (error.message.find("command failed with exit code") != std::string::npos ||
         error.message.find("completed successfully") != std::string::npos);

    if (!already_reported && !generic_runtime_exit_message) {
        print_last_error();
    }
}

const std::vector<int>& Exec::get_last_pipeline_statuses() const {
    return last_pipeline_statuses;
}
