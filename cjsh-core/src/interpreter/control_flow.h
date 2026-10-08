/*
  control_flow.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_CONTROL_FLOW_H
#define CJSH_CORE_SRC_INTERPRETER_CONTROL_FLOW_H

#include <cstdint>
#include <optional>

enum class ControlFlowKind : std::uint8_t {
    None,
    Return,
    Break,
    Continue
};

// Shell statuses always remain ordinary numbers. Only a control builtin can
// request unwinding, and only the matching function/source/loop boundary consumes
// it. This state is local to the interpreter, never a script-visible variable.
class ControlFlowState {
   public:
    bool pending() const {
        return kind_ != ControlFlowKind::None;
    }

    int status() const {
        return status_;
    }

    void request_return(int status) {
        kind_ = ControlFlowKind::Return;
        status_ = status;
        levels_ = 0;
    }

    void request_loop(ControlFlowKind kind, int levels) {
        kind_ = kind;
        status_ = 0;
        levels_ = levels;
    }

    std::optional<int> consume_return() {
        if (kind_ != ControlFlowKind::Return) {
            return std::nullopt;
        }
        const int result = status_;
        *this = {};
        return result;
    }

    ControlFlowKind consume_loop() {
        if (kind_ != ControlFlowKind::Break && kind_ != ControlFlowKind::Continue) {
            return kind_;
        }
        if (levels_ > 1) {
            --levels_;
            // continue N exits intervening loops before continuing its target.
            return ControlFlowKind::Break;
        }
        const auto result = kind_;
        *this = {};
        return result;
    }

   private:
    ControlFlowKind kind_ = ControlFlowKind::None;
    int status_ = 0;
    int levels_ = 0;
};

#endif  // CJSH_CORE_SRC_INTERPRETER_CONTROL_FLOW_H
