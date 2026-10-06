/*
  edit_distance_utils.h

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

#ifndef CJSH_CORE_SRC_UTILS_EDIT_DISTANCE_UTILS_H
#define CJSH_CORE_SRC_UTILS_EDIT_DISTANCE_UTILS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace edit_distance_utils {

inline int levenshtein_distance_with_limit(const std::string& source, const std::string& target,
                                           int max_distance) {
    if (max_distance < 0) {
        return std::numeric_limits<int>::max();
    }

    const size_t source_length = source.length();
    const size_t target_length = target.length();

    if (std::abs(static_cast<int>(source_length) - static_cast<int>(target_length)) >
        max_distance) {
        return max_distance + 1;
    }

    std::vector<int> previous_row(target_length + 1);
    std::vector<int> current_row(target_length + 1);

    for (size_t j = 0; j <= target_length; ++j) {
        previous_row[j] = static_cast<int>(j);
    }

    for (size_t i = 1; i <= source_length; ++i) {
        current_row[0] = static_cast<int>(i);
        int row_min = current_row[0];

        for (size_t j = 1; j <= target_length; ++j) {
            const int substitution_cost = (source[i - 1] == target[j - 1]) ? 0 : 1;
            current_row[j] = std::min({previous_row[j] + 1, current_row[j - 1] + 1,
                                       previous_row[j - 1] + substitution_cost});
            row_min = std::min(row_min, current_row[j]);
        }

        if (row_min > max_distance) {
            return max_distance + 1;
        }

        std::swap(previous_row, current_row);
    }

    return previous_row[target_length];
}

inline int levenshtein_distance(const std::string& source, const std::string& target) {
    return levenshtein_distance_with_limit(source, target, std::numeric_limits<int>::max() / 2);
}

}  // namespace edit_distance_utils

#endif  // CJSH_CORE_SRC_UTILS_EDIT_DISTANCE_UTILS_H
