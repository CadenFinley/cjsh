/*
  pipeline_status_utils.cpp

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

#include "pipeline_status_utils.h"

#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "exec.h"
#include "shell_env.h"

namespace {

std::string build_status_string(const std::vector<int>& statuses) {
    std::stringstream builder;
    for (size_t i = 0; i < statuses.size(); ++i) {
        if (i != 0) {
            builder << ' ';
        }
        builder << statuses[i];
    }
    return builder.str();
}

}  // namespace

namespace pipeline_status_utils {

void set_last_status_env(int status_code) {
    static thread_local bool cached = false;
    static thread_local int cached_status = 0;

    if (cached && cached_status == status_code) {
        return;
    }

    cached = true;
    cached_status = status_code;

    const std::string status_string = std::to_string(status_code);
    (void)setenv("?", status_string.c_str(), 1);
}

void apply_execution_status_env(int status_code, Exec* exec_ptr,
                                const std::function<void(const std::string&)>& on_pipe_set_callback,
                                const std::function<void()>& on_pipe_unset_callback) {
    set_last_status_env(status_code);
    apply_pipeline_status_env(exec_ptr, on_pipe_set_callback, on_pipe_unset_callback);
}

void apply_pipeline_status_env(Exec* exec_ptr,
                               const std::function<void(const std::string&)>& on_set_callback,
                               const std::function<void()>& on_unset_callback) {
    if (!exec_ptr) {
        (void)cjsh_env::unset_shell_variable_value("PIPESTATUS");
        if (on_unset_callback) {
            on_unset_callback();
        }
        return;
    }

    const auto& pipeline_statuses = exec_ptr->get_last_pipeline_statuses();
    if (pipeline_statuses.empty()) {
        (void)cjsh_env::unset_shell_variable_value("PIPESTATUS");
        if (on_unset_callback) {
            on_unset_callback();
        }
        return;
    }

    static thread_local std::vector<int> cached_statuses;
    static thread_local std::string pipe_status_str;
    if (cached_statuses != pipeline_statuses) {
        pipe_status_str = build_status_string(pipeline_statuses);
        cached_statuses = pipeline_statuses;
    }
    // Repeated publication is common at nested execution boundaries. Check the
    // live binding too: scripts can overwrite or unset PIPESTATUS between calls.
    const auto& variables = cjsh_env::env_vars();
    const auto current = variables.find("PIPESTATUS");
    if (current == variables.end() || current->second != pipe_status_str) {
        (void)cjsh_env::set_shell_variable_value("PIPESTATUS", pipe_status_str);
    }
    if (on_set_callback) {
        on_set_callback(pipe_status_str);
    }
}

}  // namespace pipeline_status_utils
