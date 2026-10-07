/*
  wait_status_utils.h

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

#ifndef CJSH_CORE_SRC_UTILS_WAIT_STATUS_UTILS_H
#define CJSH_CORE_SRC_UTILS_WAIT_STATUS_UTILS_H

namespace wait_status_utils {

enum class WaitDisposition : unsigned char {
    Exited,
    Signaled,
    Stopped,
    Other
};

struct WaitStatusInfo {
    WaitDisposition disposition = WaitDisposition::Other;
    int code = 0;
};

WaitStatusInfo decode(int status);

int to_exit_code(int status, int fallback = 1);

}  // namespace wait_status_utils

#endif  // CJSH_CORE_SRC_UTILS_WAIT_STATUS_UTILS_H
