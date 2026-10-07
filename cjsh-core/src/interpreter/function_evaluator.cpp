/*
  function_evaluator.cpp

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

#include "function_evaluator.h"
#include <memory>

#include <cctype>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "error_out.h"
#include "parser_utils.h"
#include "readonly_command.h"

namespace function_evaluator {

std::optional<FunctionHeader> parse_function_header(const std::string& source,
                                                    bool allow_missing_body) {
    auto skip_space = [&](size_t& pos) {
        while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos]))) {
            ++pos;
        }
    };
    size_t start = 0;
    skip_space(start);
    const bool keyword = source.compare(start, 8, "function") == 0 && start + 8 < source.size() &&
                         std::isspace(static_cast<unsigned char>(source[start + 8]));
    size_t name_start = start;
    size_t name_end;
    if (keyword) {
        name_start += 8;
        skip_space(name_start);
        name_end = name_start;
        while (name_end < source.size() &&
               !std::isspace(static_cast<unsigned char>(source[name_end])) &&
               source[name_end] != '(' && source[name_end] != '{') {
            ++name_end;
        }
    } else {
        name_end = source.find("()", name_start);
        if (name_end == std::string::npos) {
            return std::nullopt;
        }
    }
    size_t pos = name_end;
    while (name_end > name_start &&
           std::isspace(static_cast<unsigned char>(source[name_end - 1]))) {
        --name_end;
    }
    std::string name = source.substr(name_start, name_end - name_start);
    if (name.empty() || name.find(' ') != std::string::npos) {
        return std::nullopt;
    }
    skip_space(pos);
    if (pos < source.size() && source[pos] == '(') {
        size_t lookahead = pos + 1;
        skip_space(lookahead);
        if (lookahead < source.size() && source[lookahead] == ')') {
            pos = lookahead + 1;
            skip_space(pos);
        }
    }
    if (pos >= source.size()) {
        return allow_missing_body
                   ? std::optional<FunctionHeader>{{std::move(name), pos, '\0', '\0'}}
                   : std::nullopt;
    }
    if (source[pos] != '{' && source[pos] != '(') {
        for (const auto* keyword : {"if", "for", "while", "until", "case"}) {
            const std::string word(keyword);
            if (source.compare(pos, word.size(), word) == 0 &&
                (pos + word.size() == source.size() ||
                 std::isspace(static_cast<unsigned char>(source[pos + word.size()])))) {
                return FunctionHeader{std::move(name), pos, 'k', '\0'};
            }
        }
        return std::nullopt;
    }
    return FunctionHeader{std::move(name), pos, source[pos], source[pos] == '{' ? '}' : ')'};
}

size_t find_function_body_end(const std::string& source, const FunctionHeader& header) {
    if (header.opening != 'k') {
        size_t close = header.opening == '{' ? find_matching_brace(source, header.body_start)
                                             : find_matching_paren(source, header.body_start);
        return close == std::string::npos ? close : close + 1;
    }
    const std::string body = source.substr(header.body_start);
    const bool is_if = body.rfind("if", 0) == 0;
    const bool is_case = body.rfind("case", 0) == 0;
    const std::string closer = is_if ? "fi" : is_case ? "esac" : "done";
    const std::vector<std::string> openers =
        is_if     ? std::vector<std::string>{"if"}
        : is_case ? std::vector<std::string>{"case"}
                  : std::vector<std::string>{"for", "while", "until"};
    int depth = 0;
    size_t end = parser_find_block_end(body, openers, closer, depth);
    return end == std::string::npos ? end : header.body_start + end + closer.size();
}

FunctionParseResult parse_and_register_functions(
    const std::string& line, const std::vector<std::string>& lines, size_t& line_index,
    FunctionMap& functions, const std::function<std::string(const std::string&)>& trim_func,
    const std::function<std::string(const std::string&)>& strip_comment_func,
    const std::function<std::vector<std::string>(const std::string&)>& parse_lines_func) {
    FunctionParseResult result{false, ""};
    std::string current_line = line;
    while (!current_line.empty()) {
        auto header = parse_function_header(current_line, true);
        if (!header) {
            break;
        }
        while (header->opening == '\0' && line_index + 1 < lines.size()) {
            current_line += '\n';
            current_line += strip_comment_func(lines[++line_index]);
            header = parse_function_header(current_line, true);
            if (!header) {
                break;
            }
        }
        if (!header || header->opening == '\0') {
            break;
        }
        auto find_close = [&] { return find_function_body_end(current_line, *header); };
        size_t body_close = find_close();
        while (body_close == std::string::npos && line_index + 1 < lines.size()) {
            current_line += '\n';
            current_line += lines[++line_index];
            body_close = find_close();
        }
        if (body_close == std::string::npos) {
            break;
        }
        const bool keyword_body = header->opening == 'k';
        const size_t begin = header->body_start + (keyword_body ? 0 : 1);
        const std::string body =
            current_line.substr(begin, body_close - begin - (keyword_body ? 0 : 1));
        if (readonly_function_manager_is(header->name)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "readonly",
                         header->name + ": readonly function",
                         {}});
        } else {
            functions[header->name] = std::make_shared<FunctionDefinition>(
                FunctionDefinition{parse_lines_func(body), header->opening == '(', {}});
        }
        result.found = true;
        current_line = trim_func(current_line.substr(body_close));
        const size_t next = current_line.find_first_not_of("; \t\r\n");
        current_line = next == std::string::npos ? "" : current_line.substr(next);
    }
    result.remaining_line = std::move(current_line);
    return result;
}

bool has_function(const FunctionMap& functions, const std::string& name) {
    return functions.find(name) != functions.end();
}

std::vector<std::string> get_function_names(const FunctionMap& functions) {
    std::vector<std::string> names;
    names.reserve(functions.size());
    for (const auto& pair : functions) {
        names.push_back(pair.first);
    }
    return names;
}

}  // namespace function_evaluator
