/*
  history_file_utils.cpp

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

#include "history_file_utils.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cjsh_filesystem.h"
#include "isocline.h"

namespace history_file_utils {

std::vector<std::string> parse_history_entries(const std::string& history_content) {
    std::vector<std::string> entries;
    entries.reserve(256);

    std::stringstream content_stream(history_content);
    std::string line;
    while (std::getline(content_stream, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // Use the same decoder as the editor so multiline entries remain one event and
        // literal backslashes are restored exactly once before parsing or execution.
        std::string decoded(line.size() + 1, '\0');
        size_t decoded_length = 0;
        if (!ic_history_decode_entry(line.data(), line.size(), decoded.data(), decoded.size(),
                                     &decoded_length) ||
            decoded_length == 0) {
            continue;
        }
        decoded.resize(decoded_length);
        entries.push_back(std::move(decoded));
    }

    return entries;
}

std::vector<std::string> read_history_entries(const std::string& history_path) {
    auto read_result = cjsh_filesystem::read_file_content(history_path, true);
    if (read_result.is_error()) {
        return {};
    }

    return parse_history_entries(read_result.value());
}

}  // namespace history_file_utils
