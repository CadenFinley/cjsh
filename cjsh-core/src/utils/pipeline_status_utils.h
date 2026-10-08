/*
  pipeline_status_utils.h

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

#ifndef CJSH_CORE_SRC_UTILS_PIPELINE_STATUS_UTILS_H
#define CJSH_CORE_SRC_UTILS_PIPELINE_STATUS_UTILS_H

#include <functional>
#include <string>

class Exec;

namespace pipeline_status_utils {

void set_last_status_env(int status_code);

void apply_execution_status_env(
    int status_code, Exec* executor,
    const std::function<void(const std::string&)>& on_pipe_set_callback = {},
    const std::function<void()>& on_pipe_unset_callback = {});

void apply_pipeline_status_env(Exec* executor,
                               const std::function<void(const std::string&)>& on_set_callback = {},
                               const std::function<void()>& on_unset_callback = {});

}  // namespace pipeline_status_utils

#endif  // CJSH_CORE_SRC_UTILS_PIPELINE_STATUS_UTILS_H
