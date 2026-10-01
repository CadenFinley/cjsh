/*
  tokenizer.cpp

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

#include "tokenizer.h"

#include <pwd.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "parser_utils.h"
#include "quote_info.h"
#include "shell_env.h"

namespace {
inline bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

std::vector<std::string> Tokenizer::tokenize_command(const std::string& cmdline) {
    std::vector<std::string> tokens;
    tokens.reserve(std::min(cmdline.length() / 4 + 4, size_t(64)));
    std::string current_token;
    current_token.reserve(128);
    bool in_quotes = false;
    char quote_char = '\0';
    bool escaped = false;
    int arith_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;
    int extglob_depth = 0;
    bool in_subst_literal = false;

    bool token_saw_single = false;
    bool token_saw_double = false;
    bool token_saw_escape = false;
    bool token_saw_substitution = false;
    bool token_saw_unquoted = false;
    std::vector<size_t> io_number_tokens;

    const size_t cmdline_len = cmdline.length();

    auto flush_current_token = [&](bool before_redirection = false) {
        if (!current_token.empty() || token_saw_single || token_saw_double) {
            // An IO number must be entirely unquoted and touch the operator.
            // Record this before whitespace and quote information are discarded.
            if (before_redirection && !token_saw_single && !token_saw_double && !token_saw_escape &&
                !token_saw_substitution &&
                current_token.find_first_not_of("0123456789") == std::string::npos) {
                io_number_tokens.push_back(tokens.size());
            }
            if (token_saw_single || token_saw_double) {
                // Quoted words skip field splitting and globbing. Remove source
                // escapes now, before expansions can insert literal control bytes.
                if (token_saw_escape) {
                    current_token = remove_escape_markers(current_token);
                }
                char quote_type = token_saw_double || (config::is_posix_mode() && token_saw_unquoted)
                                      ? QUOTE_DOUBLE
                                      : QUOTE_SINGLE;
                if (config::is_posix_mode() && quote_type == QUOTE_SINGLE) {
                    current_token = strip_noenv_sentinels(current_token).first;
                }
                tokens.push_back(create_quote_tag(quote_type, current_token));
            } else {
                tokens.push_back(current_token);
            }
            current_token.clear();
            token_saw_single = token_saw_double = false;
            token_saw_escape = token_saw_substitution = false;
            token_saw_unquoted = false;
        }
    };

    const std::string& subst_start = subst_literal_start();
    const std::string& subst_end = subst_literal_end();

    for (size_t i = 0; i < cmdline_len; ++i) {
        if (!in_subst_literal && cmdline.compare(i, subst_start.size(), subst_start) == 0) {
            in_subst_literal = true;
            token_saw_substitution = true;
            i += subst_start.size() - 1;
            continue;
        }

        if (in_subst_literal && cmdline.compare(i, subst_end.size(), subst_end) == 0) {
            in_subst_literal = false;
            i += subst_end.size() - 1;
            continue;
        }

        char c = cmdline[i];

        if (in_subst_literal) {
            current_token += c;
            continue;
        }

        if (escaped) {
            if (in_quotes && quote_char == '"') {
                if (c == '$') {
                    current_token += '\\';
                    current_token += c;
                } else if (c == '`' || c == '"' || c == '\\' || c == '\n') {
                    current_token += c;
                } else {
                    current_token += '\\';
                    current_token += c;
                }
            } else {
                // Keep escaped whitespace protected until field splitting has finished.
                if (is_whitespace(c) || c == '*' || c == '?' || c == '[' || c == ']') {
                    current_token += '\x1F';
                }
                current_token += c;
            }
            escaped = false;
        } else if (!in_subst_literal && c == '\\' && (!in_quotes || quote_char != '\'')) {
            escaped = true;
            token_saw_escape = true;
        } else if ((c == '"' || c == '\'') && !in_quotes && !in_subst_literal) {
            in_quotes = true;
            quote_char = c;
            if (c == '\'') {
                token_saw_single = true;
                if (config::is_posix_mode()) {
                    current_token += noenv_start();
                }
            } else {
                token_saw_double = true;
            }
        } else if (c == quote_char && in_quotes && !in_subst_literal) {
            if (config::is_posix_mode() && quote_char == '\'') {
                current_token += noenv_end();
            }
            in_quotes = false;
            quote_char = '\0';
        } else if (!in_quotes) {
            // Resolve tilde from source spelling, before parameter expansion and
            // quote removal. Protect only the inserted home directory.
            if (config::is_posix_mode() && c == '~' && brace_depth == 0 && arith_depth == 0) {
                const size_t assignment_eq = current_token.find('=');
                const bool assignment_prefix =
                    assignment_eq != std::string::npos &&
                    cjsh_env::is_valid_env_name(current_token.substr(0, assignment_eq)) &&
                    (current_token.back() == '=' || current_token.back() == ':') &&
                    (std::all_of(tokens.begin(), tokens.end(),
                                 [](const std::string& token) {
                                     return looks_like_assignment(QuoteInfo(token).value);
                                 }) ||
                     (!tokens.empty() && (tokens[0] == "export" || tokens[0] == "readonly")));
                if (current_token.empty() || assignment_prefix) {
                    size_t end = i + 1;
                    while (end < cmdline_len && !is_whitespace(cmdline[end]) &&
                           std::string_view("/:;|&<>()").find(cmdline[end]) ==
                               std::string_view::npos) {
                        ++end;
                    }
                    const std::string login = cmdline.substr(i + 1, end - i - 1);
                    std::optional<std::string> directory;
                    if (login.empty() && cjsh_env::shell_variable_is_set("HOME")) {
                        directory = cjsh_env::get_shell_variable_value("HOME");
                    } else if (!login.empty()) {
                        if (const passwd* entry = getpwnam(login.c_str())) {
                            directory = entry->pw_dir;
                        }
                    }
                    if (directory) {
                        current_token += noenv_start();
                        const std::string protected_chars =
                            cjsh_env::get_ifs_delimiters() + "*?[]\\";
                        for (const char ch : *directory) {
                            if (protected_chars.find(ch) != std::string::npos) {
                                current_token += QUOTE_PREFIX;
                                token_saw_escape = true;
                            }
                            current_token += ch;
                        }
                        current_token += noenv_end();
                        token_saw_unquoted = true;
                        i = end - 1;
                        continue;
                    }
                }
            }
            if (extglob_depth > 0) {
                if (c == '(') {
                    ++extglob_depth;
                } else if (c == ')') {
                    --extglob_depth;
                }
                current_token += c;
            }

            else if (config::extglob_enabled && c == '(' && !current_token.empty() &&
                     (current_token.back() == '?' || current_token.back() == '*' ||
                      current_token.back() == '+' || current_token.back() == '@' ||
                      current_token.back() == '!')) {
                extglob_depth = 1;
                current_token += c;
            }

            else if (c == '{' && current_token.length() >= 1 && current_token.back() == '$') {
                brace_depth++;
                current_token += c;
            }

            else if (c == '}' && brace_depth > 0) {
                brace_depth--;
                current_token += c;
            }

            else if (is_whitespace(c)) {
                if ((!current_token.empty() || token_saw_single || token_saw_double) &&
                    arith_depth == 0 && brace_depth == 0) {
                    flush_current_token();
                } else if (arith_depth > 0 || brace_depth > 0) {
                    current_token += c;
                }
            }

            else if (c == '(' && i >= 1 && cmdline[i - 1] == '(' && current_token.length() >= 1 &&
                     current_token.back() == '$') {
                arith_depth++;
                current_token += c;
            }

            else if (c == ')' && i + 1 < cmdline_len && cmdline[i + 1] == ')' && arith_depth > 0) {
                arith_depth--;
                current_token += c;
                current_token += cmdline[i + 1];
                i++;
            }

            else if (c == '[' && i + 1 < cmdline_len && cmdline[i + 1] == '[' &&
                     (!config::is_posix_mode() ||
                      (current_token.empty() &&
                       (i + 2 == cmdline_len || is_whitespace(cmdline[i + 2]))))) {
                bracket_depth++;
                flush_current_token();
                tokens.push_back("[[");
                i++;
            }

            else if (c == ']' && i + 1 < cmdline_len && cmdline[i + 1] == ']' &&
                     bracket_depth > 0) {
                bracket_depth--;
                flush_current_token();
                tokens.push_back("]]");
                i++;
            }

            else if (bracket_depth > 0 && i + 1 < cmdline_len &&
                     ((c == '&' && cmdline[i + 1] == '&') || (c == '|' && cmdline[i + 1] == '|'))) {
                flush_current_token();

                (void)tokens.emplace_back(2, c);
                tokens.back()[1] = cmdline[i + 1];
                i++;
            }

            else if ((c == '(' || c == ')' || c == '<' || c == '>' ||
                      (c == '&' && arith_depth == 0 && brace_depth == 0 && bracket_depth == 0) ||
                      (c == '|' && arith_depth == 0 && brace_depth == 0 && bracket_depth == 0))) {
                const bool before_redirection =
                    (c == '<' || c == '>') && !(i + 1 < cmdline_len && cmdline[i + 1] == '(');
                flush_current_token(before_redirection);

                bool handled_special = false;
                if ((c == '<' || c == '>') && i + 1 < cmdline_len && cmdline[i + 1] == '(') {
                    size_t j = i + 2;
                    int paren_depth = 1;
                    bool in_single = false;
                    bool in_double = false;

                    while (j < cmdline_len) {
                        char ch = cmdline[j];
                        if (!in_double && ch == '\'' && (j == i + 2 || cmdline[j - 1] != '\\')) {
                            in_single = !in_single;
                        } else if (!in_single && ch == '"' &&
                                   (j == i + 2 || cmdline[j - 1] != '\\')) {
                            in_double = !in_double;
                        } else if (!in_single && !in_double) {
                            if (ch == '(') {
                                paren_depth++;
                            } else if (ch == ')') {
                                paren_depth--;
                                if (paren_depth == 0) {
                                    break;
                                }
                            }
                        }

                        if (ch == '\\' && !in_single) {
                            j += 2;
                            continue;
                        }
                        j++;
                    }

                    if (paren_depth == 0 && j < cmdline_len) {
                        tokens.push_back(cmdline.substr(i, j - i + 1));
                        i = j;
                        handled_special = true;
                    }
                }

                if (handled_special) {
                    continue;
                }

                if ((c == '<' || c == '>') && i + 1 < cmdline_len && cmdline[i + 1] == '&') {
                    size_t j = i + 2;
                    while (j < cmdline_len &&
                           ((std::isdigit(static_cast<unsigned char>(cmdline[j])) != 0) ||
                            cmdline[j] == '-')) {
                        j++;
                    }
                    if (j > i + 2) {
                        tokens.push_back(cmdline.substr(i, j - i));
                        i = j - 1;
                        continue;
                    }
                }
                if (c == '&' && !tokens.empty() &&
                    (tokens.back() == "<" || tokens.back() == ">" || tokens.back() == ">>") &&
                    i + 1 < cmdline_len) {
                    size_t j = i + 1;
                    while (j < cmdline_len &&
                           ((std::isdigit(static_cast<unsigned char>(cmdline[j])) != 0) ||
                            cmdline[j] == '-')) {
                        j++;
                    }
                    if (j > i + 1) {
                        size_t merged_size = tokens.back().size() + 1 + (j - i - 1);
                        std::string merged;
                        merged.reserve(merged_size);
                        merged = tokens.back();
                        merged += '&';
                        (void)merged.append(cmdline, i + 1, j - i - 1);
                        tokens.back() = std::move(merged);
                        i = j - 1;
                        continue;
                    }
                }
                (void)tokens.emplace_back(1, c);
            } else {
                token_saw_unquoted = true;
                current_token += c;
            }
        } else {
            current_token += c;
        }
    }

    flush_current_token();

    if (in_quotes) {
        throw std::runtime_error("cjsh: Unclosed quote: missing closing " +
                                 std::string(1, quote_char));
    }

    return merge_redirection_tokens(tokens, io_number_tokens);
}

std::vector<std::string> Tokenizer::merge_redirection_tokens(
    const std::vector<std::string>& tokens, const std::vector<size_t>& io_number_tokens) {
    std::vector<std::string> result;
    result.reserve(tokens.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        const bool is_io_number =
            std::binary_search(io_number_tokens.begin(), io_number_tokens.end(), i);

        if (is_io_number && token == "2") {
            size_t next_index = i + 1;
            if (next_index >= tokens.size()) {
                result.push_back(token);
                continue;
            }
            const std::string& next_token = tokens[next_index];

            if (next_token == ">&1") {
                result.push_back("2>&1");
                i++;
            } else if (i + 3 < tokens.size() && next_token == ">" && tokens[i + 2] == "&" &&
                       tokens[i + 3] == "1") {
                result.push_back("2>&1");
                i += 3;
            } else if (i + 2 < tokens.size() && next_token == ">" && tokens[i + 2] == ">") {
                result.push_back("2>>");
                i += 2;
            } else if (next_token == ">") {
                result.push_back("2>");
                i++;
            } else {
                result.push_back(token);
            }
        }

        else if (token == "&" && i + 1 < tokens.size() && tokens[i + 1] == ">") {
            result.push_back("&>");
            i++;
        }

        else if (token == ">" && i + 1 < tokens.size() && tokens[i + 1] == "|") {
            result.push_back(">|");
            i++;
        }

        else if (token == "<" && i + 2 < tokens.size() && tokens[i + 1] == "<" &&
                 tokens[i + 2] == "<") {
            result.push_back("<<<");
            i += 2;
        }

        else if (token == "<" && i + 2 < tokens.size() && tokens[i + 1] == "<" &&
                 tokens[i + 2] == "-") {
            result.push_back("<<-");
            i += 2;
        }

        else if (token == "<" && i + 1 < tokens.size() && tokens[i + 1] == "<") {
            result.push_back("<<");
            i++;
        }

        else if (token == "<<" && i + 1 < tokens.size() && tokens[i + 1] == "<") {
            result.push_back("<<<");
            i++;
        }

        else if (token == "<<" && i + 1 < tokens.size() && tokens[i + 1] == "-") {
            result.push_back("<<-");
            i++;
        }

        else if (token == "<" && i + 2 < tokens.size() && tokens[i + 1] == "&" &&
                 !tokens[i + 2].empty() &&
                 ((std::isdigit(static_cast<unsigned char>(tokens[i + 2][0])) != 0) ||
                  tokens[i + 2][0] == '$')) {
            result.push_back("<&" + tokens[i + 2]);
            i += 2;
        }

        else if (token == ">" && i + 2 < tokens.size() && tokens[i + 1] == "&" &&
                 !tokens[i + 2].empty() &&
                 ((std::isdigit(static_cast<unsigned char>(tokens[i + 2][0])) != 0) ||
                  tokens[i + 2][0] == '$')) {
            result.push_back(">&" + tokens[i + 2]);
            i += 2;
        }

        else if (token == ">" && i + 1 < tokens.size() && tokens[i + 1] == ">") {
            result.push_back(">>");
            i++;
        }

        else if ((token == ">>" || token == ">") && i + 1 < tokens.size() &&
                 (tokens[i + 1] == "&1" || tokens[i + 1] == "&2")) {
            result.push_back(token + tokens[i + 1]);
            i++;
        }

        else if (token == ">" && i + 2 < tokens.size() && tokens[i + 1] == "&" &&
                 tokens[i + 2] == "2") {
            result.push_back(">&2");
            i += 2;
        }

        else if (token == ">" && i + 2 < tokens.size() && tokens[i + 1] == "&" &&
                 tokens[i + 2] == "1") {
            result.push_back(">&1");
            i += 2;
        }

        else if (is_io_number && token == "2" && i + 2 < tokens.size() && tokens[i + 1] == "&" &&
                 tokens[i + 2] == "1") {
            result.push_back("2>&1");
            i += 2;
        }

        else if (is_io_number && token == "2" && i + 1 < tokens.size() &&
                 (tokens[i + 1] == ">" || tokens[i + 1] == ">>")) {
            result.push_back("2" + tokens[i + 1]);
            i++;
        }

        else if (is_io_number && token.length() == 1 && i + 1 < tokens.size() &&
                 (tokens[i + 1] == "<" || tokens[i + 1] == ">")) {
            result.push_back(token + tokens[i + 1]);
            i++;
        }

        else if (is_io_number && token.find_first_not_of("0123456789") == std::string::npos &&
                 i + 1 < tokens.size() &&
                 (tokens[i + 1].rfind("<&", 0) == 0 || tokens[i + 1].rfind(">&", 0) == 0)) {
            result.push_back(token + tokens[i + 1]);
            i++;
        }

        else if ((std::isdigit(token[0]) != 0) && token.length() > 1) {
            size_t first_non_digit = 0;
            while (first_non_digit < token.length() &&
                   (std::isdigit(token[first_non_digit]) != 0)) {
                first_non_digit++;
            }
            if (first_non_digit > 0 && first_non_digit < token.length()) {
                std::string rest = token.substr(first_non_digit);
                if (rest == "<" || rest == ">" || rest.find(">&") == 0) {
                    result.push_back(token);
                } else {
                    result.push_back(token);
                }
            } else {
                result.push_back(token);
            }
        } else {
            result.push_back(token);
        }
    }

    return result;
}

std::vector<std::string> Tokenizer::split_by_ifs(const std::string& input) {
    return split_by_ifs(input, cjsh_env::get_ifs_delimiters());
}

std::vector<std::string> Tokenizer::split_by_ifs(const std::string& input, const std::string& ifs) {
    std::vector<std::string> result;

    if (input.empty()) {
        return result;
    }

    if (looks_like_assignment(input)) {
        result.push_back(input);
        return result;
    }

    if (ifs.empty()) {
        result.push_back(input);
        return result;
    }

    std::string current_word;
    bool in_word = false;
    int process_depth = 0;
    bool in_single = false;
    bool in_double = false;

    size_t idx = 0;
    while (idx < input.size()) {
        char c = input[idx];

        if (process_depth == 0) {
            if (c == QUOTE_PREFIX && idx + 1 < input.size()) {
                current_word += c;
                current_word += input[idx + 1];
                in_word = true;
                idx += 2;
                continue;
            }

            if ((c == '<' || c == '>') && idx + 1 < input.size() && input[idx + 1] == '(') {
                if (!in_word) {
                    current_word.clear();
                }
                current_word += c;
                current_word += '(';
                in_word = true;
                process_depth = 1;
                idx += 2;
                continue;
            }

            const bool is_ifs = ifs.find(c) != std::string::npos;
            const bool is_ifs_whitespace = is_ifs && (c == ' ' || c == '\t' || c == '\n');
            if (is_ifs_whitespace) {
                if (in_word) {
                    result.push_back(current_word);
                    current_word.clear();
                    in_word = false;
                }

                do {
                    ++idx;
                } while (idx < input.size() && ifs.find(input[idx]) != std::string::npos &&
                         (input[idx] == ' ' || input[idx] == '\t' || input[idx] == '\n'));

                if (idx < input.size() && ifs.find(input[idx]) != std::string::npos &&
                    input[idx] != ' ' && input[idx] != '\t' && input[idx] != '\n') {
                    ++idx;
                    while (idx < input.size() && ifs.find(input[idx]) != std::string::npos &&
                           (input[idx] == ' ' || input[idx] == '\t' || input[idx] == '\n')) {
                        ++idx;
                    }
                }
                continue;
            }

            if (is_ifs) {
                result.push_back(current_word);
                current_word.clear();
                in_word = false;
                ++idx;
                while (idx < input.size() && ifs.find(input[idx]) != std::string::npos &&
                       (input[idx] == ' ' || input[idx] == '\t' || input[idx] == '\n')) {
                    ++idx;
                }
                continue;
            }
        } else {
            if (!in_double && c == '\'' && (idx == 0 || input[idx - 1] != '\\')) {
                in_single = !in_single;
                current_word += c;
                idx++;
                continue;
            }
            if (!in_single && c == '"' && (idx == 0 || input[idx - 1] != '\\')) {
                in_double = !in_double;
                current_word += c;
                idx++;
                continue;
            }
            if (!in_single && !in_double) {
                if (c == '(') {
                    process_depth++;
                } else if (c == ')') {
                    process_depth--;
                    if (process_depth == 0) {
                        in_single = false;
                        in_double = false;
                    }
                }
            }
            if (c == '\\' && !in_single && idx + 1 < input.size()) {
                current_word += c;
                current_word += input[idx + 1];
                idx += 2;
                continue;
            }
        }

        current_word += c;
        in_word = true;
        idx++;
    }

    if (in_word) {
        result.push_back(current_word);
    }

    return result;
}

bool Tokenizer::looks_like_assignment(const std::string& input) {
    return ::looks_like_assignment(input);
}
