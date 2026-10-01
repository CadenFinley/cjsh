/*
  command_preprocessor.cpp

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

#include "command_preprocessor.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "parser_utils.h"
#include "string_utils.h"

CommandPreprocessor::PreprocessedCommand CommandPreprocessor::preprocess(
    const std::string& command) {
    PreprocessedCommand result;
    result.processed_text = command;

    result.processed_text = process_here_documents(result.processed_text, result.here_documents);

    std::string original_text = result.processed_text;
    result.processed_text = process_subshells(result.processed_text);
    result.has_subshells = (original_text != result.processed_text);

    result.needs_special_handling = !result.here_documents.empty() || result.has_subshells;
    return result;
}

std::string CommandPreprocessor::process_here_documents(
    const std::string& command, std::map<std::string, std::string>& here_docs, bool* incomplete) {
    if (incomplete) {
        *incomplete = false;
    }
    std::string result;
    size_t position = 0;
    while (position < command.size()) {
        const size_t newline = command.find('\n', position);
        const size_t line_end = newline == std::string::npos ? command.size() : newline;
        const std::string line = command.substr(position, line_end - position);
        struct Pending {
            size_t position;
            HereDocHeader header;
            std::string placeholder;
        };
        std::vector<Pending> pending;
        char quote = '\0';
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '\\' && quote != '\'') {
                ++i;
                continue;
            }
            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                }
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                continue;
            }
            if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
                break;
            }
            if (line.compare(i, 3, "<<<") == 0) {
                i += 2;
                continue;
            }
            HereDocHeader header;
            if (c == '<' && parse_here_doc_header(line, i, header)) {
                pending.push_back(
                    {i, header, "HEREDOC_PLACEHOLDER_" + std::to_string(next_placeholder_id())});
                i = header.delimiter_end - 1;
            }
        }
        if (pending.empty() || newline == std::string::npos) {
            if (incomplete && !pending.empty()) {
                *incomplete = true;
            }
            result += line;
            if (newline != std::string::npos) {
                result += '\n';
            }
            position = newline == std::string::npos ? command.size() : newline + 1;
            continue;
        }
        size_t body_position = newline + 1;
        bool complete = true;
        for (const auto& item : pending) {
            std::string content;
            bool terminated = false;
            while (body_position < command.size()) {
                std::string logical_line;
                bool has_newline;
                do {
                    size_t end = command.find('\n', body_position);
                    has_newline = end != std::string::npos;
                    if (!has_newline) {
                        end = command.size();
                    }
                    std::string part = command.substr(body_position, end - body_position);
                    body_position = has_newline ? end + 1 : end;
                    if (item.header.strip_tabs) {
                        size_t first = part.find_first_not_of('\t');
                        part.erase(0, first == std::string::npos ? part.size() : first);
                    }
                    logical_line += part;
                    size_t slashes = 0;
                    for (size_t i = logical_line.size(); i > 0 && logical_line[i - 1] == '\\';
                         --i) {
                        ++slashes;
                    }
                    if (!item.header.expand || !has_newline || slashes % 2 == 0) {
                        break;
                    }
                    logical_line.pop_back();
                } while (body_position < command.size());
                if (logical_line == item.header.delimiter) {
                    terminated = true;
                    break;
                }
                content += logical_line;
                if (has_newline) {
                    content += '\n';
                }
            }
            if (!terminated) {
                complete = false;
                break;
            }
            here_docs[item.placeholder] = item.header.expand ? "__EXPAND__" + content : content;
        }
        if (!complete) {
            if (incomplete) {
                *incomplete = true;
            }
            result += command.substr(position);
            break;
        }
        size_t copied = 0;
        for (const auto& item : pending) {
            result += line.substr(copied, item.position - copied);
            result += "< " + item.placeholder;
            copied = item.header.delimiter_end;
        }
        result += line.substr(copied);
        result += '\n';
        position = body_position;
    }
    return result;
}

std::string CommandPreprocessor::process_subshells(const std::string& command) {
    std::string result = command;

    if (result.empty()) {
        return result;
    }
    size_t lead = result.find_first_not_of(" \t\r\n");
    if (lead == std::string::npos || (result[lead] != '(' && result[lead] != '{')) {
        return result;
    }

    size_t close_pos = std::string::npos;

    const bool is_paren_group = result[lead] == '(';

    if (is_paren_group) {
        close_pos = find_matching_paren(result, lead);
    } else if (result[lead] == '{') {
        close_pos = find_matching_brace(result, lead);
    }

    if (close_pos == std::string::npos) {
        return result;
    }

    std::string subshell_content = result.substr(lead + 1, close_pos - (lead + 1));
    std::string remaining = result.substr(close_pos + 1);

    if (!is_paren_group) {
        subshell_content = string_utils::trim_ascii_whitespace_copy(subshell_content);

        if (!subshell_content.empty() && subshell_content.back() == ';') {
            subshell_content.pop_back();
            subshell_content = string_utils::trim_right_ascii_whitespace_copy(subshell_content);
        }
    }

    std::string prefix = result.substr(0, lead);
    const std::string marker = is_paren_group ? "SUBSHELL{" : "BRACEGROUP{";
    result = prefix + marker + subshell_content + "}" + remaining;

    return result;
}

std::uint32_t CommandPreprocessor::next_placeholder_id() {
    static std::uint32_t counter = 0;
    if (counter == std::numeric_limits<std::uint32_t>::max()) {
        counter = 0;
    }
    return ++counter;
}
