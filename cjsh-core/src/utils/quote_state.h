/*
  quote_state.h

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

#ifndef CJSH_CORE_SRC_UTILS_QUOTE_STATE_H
#define CJSH_CORE_SRC_UTILS_QUOTE_STATE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace utils {

enum class QuoteAdvanceResult : std::uint8_t {
    Continue,
    Process
};

struct QuoteState {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    QuoteAdvanceResult consume_forward(char c) {
        if (escaped) {
            escaped = false;
            return QuoteAdvanceResult::Continue;
        }

        if (c == '\\' && !in_single_quote) {
            escaped = true;
            return QuoteAdvanceResult::Continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            return QuoteAdvanceResult::Continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            return QuoteAdvanceResult::Continue;
        }

        return QuoteAdvanceResult::Process;
    }

    bool inside_quotes() const {
        return in_single_quote || in_double_quote;
    }
};

struct ShellQuoteState {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_backtick = false;
    bool escaped = false;

    QuoteAdvanceResult consume_forward(char c) {
        if (escaped) {
            escaped = false;
            return QuoteAdvanceResult::Continue;
        }
        if (c == '\\' && !in_single_quote) {
            escaped = true;
            return QuoteAdvanceResult::Continue;
        }
        if (c == '\'' && !in_double_quote && !in_backtick) {
            in_single_quote = !in_single_quote;
            return QuoteAdvanceResult::Continue;
        }
        if (c == '"' && !in_single_quote && !in_backtick) {
            in_double_quote = !in_double_quote;
            return QuoteAdvanceResult::Continue;
        }
        if (c == '`' && !in_single_quote && !in_double_quote) {
            in_backtick = !in_backtick;
            return QuoteAdvanceResult::Continue;
        }
        return QuoteAdvanceResult::Process;
    }

    bool inside_quotes() const {
        return in_single_quote || in_double_quote || in_backtick;
    }
};

inline bool is_inside_quotes_at(std::string_view text, size_t pos) {
    QuoteState state;
    size_t limit = std::min(pos, text.size());
    for (size_t i = 0; i < limit; ++i) {
        (void)state.consume_forward(text[i]);
    }
    return state.inside_quotes();
}

}  // namespace utils

#endif  // CJSH_CORE_SRC_UTILS_QUOTE_STATE_H
