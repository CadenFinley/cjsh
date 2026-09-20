/*
  expansion_engine.cpp

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

#include "expansion_engine.h"

#include <fnmatch.h>
#include <glob.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "pattern_matcher.h"
#include "quote_info.h"
#include "shell.h"
#include "shell_env.h"

namespace {

struct GlobComponent {
    std::string text;
    bool is_globstar{false};
    bool has_wildcards{false};
    bool has_extglob{false};
    bool allows_hidden{false};
};

struct ParsedGlobPattern {
    bool absolute{false};
    bool trailing_slash{false};
    bool contains_globstar{false};
    bool contains_extglob{false};
    std::vector<GlobComponent> components;
};

bool component_has_wildcards(const std::string& text) {
    return text.find_first_of("*?[") != std::string::npos;
}

bool component_has_extglob(const std::string& text) {
    if (!config::extglob_enabled) {
        return false;
    }
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if ((text[i] == '?' || text[i] == '*' || text[i] == '+' || text[i] == '@' ||
             text[i] == '!') &&
            text[i + 1] == '(') {
            return true;
        }
    }
    return false;
}

bool is_hidden_name(const std::string& name) {
    return !name.empty() && name[0] == '.';
}

void append_component(ParsedGlobPattern& pattern, std::string component) {
    if (component.empty()) {
        return;
    }
    GlobComponent parsed_component;
    parsed_component.text = std::move(component);
    parsed_component.is_globstar = parsed_component.text == "**";
    parsed_component.has_extglob = component_has_extglob(parsed_component.text);
    parsed_component.has_wildcards = parsed_component.is_globstar ||
                                     component_has_wildcards(parsed_component.text) ||
                                     parsed_component.has_extglob;
    parsed_component.allows_hidden =
        !parsed_component.text.empty() && parsed_component.text.front() == '.';
    pattern.contains_globstar = pattern.contains_globstar || parsed_component.is_globstar;
    pattern.contains_extglob = pattern.contains_extglob || parsed_component.has_extglob;
    pattern.components.push_back(std::move(parsed_component));
}

ParsedGlobPattern parse_glob_pattern(const std::string& pattern) {
    ParsedGlobPattern parsed;
    if (pattern.empty()) {
        return parsed;
    }

    parsed.absolute = pattern.front() == '/';
    parsed.trailing_slash = pattern.size() > 1 && pattern.back() == '/';

    size_t start_index = parsed.absolute ? 1 : 0;
    std::string current;
    current.reserve(pattern.size());

    for (size_t i = start_index; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == '/') {
            append_component(parsed, current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    append_component(parsed, current);
    return parsed;
}

std::string format_match_path(const std::filesystem::path& path, bool absolute) {
    std::string value = path.string();
    if (!absolute) {
        if (value == ".") {
            // keep
        } else if (value.rfind("./", 0) == 0) {
            (void)value.erase(0, 2);
        }
    }

    if (value.empty()) {
        value = ".";
    }

    return value;
}

bool path_is_directory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

void append_match(const std::filesystem::path& path, bool absolute,
                  std::vector<std::string>& matches) {
    std::string formatted = format_match_path(path, absolute);

    if (path_is_directory(path) &&
        (formatted != "/" && (formatted.empty() || formatted.back() != '/'))) {
        formatted.push_back('/');
    }

    matches.push_back(std::move(formatted));
}

void globstar_recurse(const ParsedGlobPattern& pattern, size_t index,
                      const std::filesystem::path& current_path,
                      std::vector<std::string>& matches) {
    if (index >= pattern.components.size()) {
        if (pattern.trailing_slash && !path_is_directory(current_path)) {
            return;
        }
        append_match(current_path, pattern.absolute, matches);
        return;
    }

    const auto& component = pattern.components[index];

    if (component.is_globstar) {
        globstar_recurse(pattern, index + 1, current_path, matches);

        if (!path_is_directory(current_path)) {
            return;
        }

        std::error_code iter_ec;
        std::filesystem::directory_iterator dir_it(
            current_path, std::filesystem::directory_options::skip_permission_denied, iter_ec);
        if (iter_ec) {
            return;
        }

        for (const auto& entry : dir_it) {
            std::string name = entry.path().filename().string();
            if (!component.allows_hidden && is_hidden_name(name)) {
                continue;
            }

            std::error_code dir_entry_ec;
            bool is_dir = entry.is_directory(dir_entry_ec);
            if (!is_dir) {
                continue;
            }

            std::error_code symlink_ec;
            bool is_symlink = entry.is_symlink(symlink_ec);

            if (is_symlink && index + 1 < pattern.components.size()) {
                continue;
            }

            if (is_symlink) {
                globstar_recurse(pattern, index + 1, entry.path(), matches);
                continue;
            }

            globstar_recurse(pattern, index, entry.path(), matches);
        }
        return;
    }

    if (!component.has_wildcards) {
        auto next_path = current_path / component.text;
        std::error_code exists_ec;
        if (!std::filesystem::exists(next_path, exists_ec)) {
            return;
        }
        globstar_recurse(pattern, index + 1, next_path, matches);
        return;
    }

    if (!path_is_directory(current_path)) {
        return;
    }

    std::error_code iter_ec;
    std::filesystem::directory_iterator dir_it(
        current_path, std::filesystem::directory_options::skip_permission_denied, iter_ec);
    if (iter_ec) {
        return;
    }

    int fnmatch_flags = FNM_PERIOD;
    PatternMatcher pattern_matcher;
    for (const auto& entry : dir_it) {
        std::string name = entry.path().filename().string();
        if (!component.allows_hidden && is_hidden_name(name)) {
            continue;
        }
        bool component_matches =
            component.has_extglob
                ? pattern_matcher.matches_pattern(name, component.text)
                : fnmatch(component.text.c_str(), name.c_str(), fnmatch_flags) == 0;
        if (component_matches) {
            globstar_recurse(pattern, index + 1, entry.path(), matches);
        }
    }
}

std::vector<std::string> expand_globstar_pattern(const ParsedGlobPattern& pattern) {
    std::vector<std::string> matches;
    std::filesystem::path base =
        pattern.absolute ? std::filesystem::path("/") : std::filesystem::path(".");

    if (pattern.components.empty()) {
        append_match(base, pattern.absolute, matches);
        return matches;
    }

    globstar_recurse(pattern, 0, base, matches);
    std::sort(matches.begin(), matches.end());
    (void)matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

}  // namespace

ExpansionEngine::ExpansionEngine(Shell* shell) : shell(shell) {
}

std::vector<std::string> ExpansionEngine::expand_braces(const std::string& pattern) {
    std::vector<std::string> result;

    result.reserve(8);

    if (config::posix_mode) {
        result.push_back(pattern);
        return result;
    }

    size_t open_pos = pattern.find('{');
    if (open_pos == std::string::npos) {
        result.push_back(pattern);
        return result;
    }

    int depth = 1;
    size_t close_pos = open_pos + 1;

    while (close_pos < pattern.size() && depth > 0) {
        if (pattern[close_pos] == '{') {
            depth++;
        } else if (pattern[close_pos] == '}') {
            depth--;
        }

        if (depth > 0) {
            close_pos++;
        }
    }

    if (depth != 0) {
        result.push_back(pattern);
        return result;
    }

    std::string prefix = pattern.substr(0, open_pos);
    std::string content = pattern.substr(open_pos + 1, close_pos - open_pos - 1);
    std::string suffix = pattern.substr(close_pos + 1);

    if (content.empty() && prefix.empty() && suffix.empty()) {
        result.push_back(pattern);
        return result;
    }

    size_t range_pos = content.find("..");
    if (range_pos != std::string::npos) {
        std::string start_str = content.substr(0, range_pos);
        std::string range_tail = content.substr(range_pos + 2);
        size_t stride_pos = range_tail.find("..");
        std::string end_str = range_tail.substr(0, stride_pos);
        std::string stride_str =
            stride_pos == std::string::npos ? std::string{} : range_tail.substr(stride_pos + 2);

        auto is_numeric = [](const std::string& str) {
            if (str.empty()) {
                return false;
            }
            size_t start = 0;
            if (str[0] == '-' || str[0] == '+') {
                start = 1;
            }
            if (start >= str.length()) {
                return false;
            }
            auto start_it = str.begin();
            std::advance(start_it, static_cast<std::string::difference_type>(start));
            return std::all_of(start_it, str.end(), [](char c) { return std::isdigit(c); });
        };

        if (stride_pos != std::string::npos &&
            (stride_str.empty() || !is_numeric(stride_str) ||
             range_tail.find("..", stride_pos + 2) != std::string::npos)) {
            result.push_back(pattern);
            return result;
        }

        bool is_numeric_range = is_numeric(start_str) && is_numeric(end_str);

        if (is_numeric_range) {
            int start_int = 0;
            int end_int = 0;
            int stride = 1;
            try {
                start_int = std::stoi(start_str);
                end_int = std::stoi(end_str);
                if (!stride_str.empty()) {
                    stride = std::stoi(stride_str);
                }
            } catch (const std::exception&) {
                result.push_back(pattern);
                return result;
            }
            if (stride == 0) {
                stride = 1;
            }
            stride = std::abs(stride);
            const long long distance =
                std::llabs(static_cast<long long>(end_int) - static_cast<long long>(start_int));
            size_t range_size = static_cast<size_t>(distance / stride) + 1U;
            if (range_size > MAX_EXPANSION_SIZE) {
                result.push_back(pattern);
                return result;
            }
            auto digit_width = [](const std::string& value) {
                size_t offset = (!value.empty() && (value.front() == '-' || value.front() == '+'));
                return value.size() - offset;
            };
            const bool preserve_width =
                (digit_width(start_str) > 1 &&
                 start_str[start_str.front() == '-' || start_str.front() == '+'] == '0') ||
                (digit_width(end_str) > 1 &&
                 end_str[end_str.front() == '-' || end_str.front() == '+'] == '0');
            const size_t numeric_width =
                preserve_width ? std::max(digit_width(start_str), digit_width(end_str)) : 0;
            result.reserve(range_size);
            expand_range(start_int, end_int, prefix, suffix, result, stride, numeric_width);
            return result;
        }

        if (start_str.length() == 1 && end_str.length() == 1 && (std::isalpha(start_str[0]) != 0) &&
            (std::isalpha(end_str[0]) != 0)) {
            char start_char = start_str[0];
            char end_char = end_str[0];

            bool both_lower = std::islower(start_char) && std::islower(end_char);
            bool both_upper = std::isupper(start_char) && std::isupper(end_char);

            if (both_lower || both_upper) {
                int stride = 1;
                if (!stride_str.empty()) {
                    try {
                        stride = std::stoi(stride_str);
                    } catch (const std::exception&) {
                        result.push_back(pattern);
                        return result;
                    }
                }
                if (stride == 0) {
                    stride = 1;
                }
                stride = std::abs(stride);
                int char_diff = end_char - start_char;
                size_t char_range_size = static_cast<size_t>(std::abs(char_diff) / stride) + 1U;
                if (char_range_size > MAX_EXPANSION_SIZE) {
                    result.push_back(pattern);
                    return result;
                }
                result.reserve(char_range_size);
                expand_range(start_char, end_char, prefix, suffix, result,
                             static_cast<char>(stride));
                return result;
            }
        }

        result.push_back(pattern);
        return result;
    }

    std::vector<std::string> options;
    size_t start = 0;
    depth = 0;

    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || (content[i] == ',' && depth == 0)) {
            (void)options.emplace_back(content.substr(start, i - start));
            start = i + 1;
        } else if (content[i] == '{') {
            depth++;
        } else if (content[i] == '}') {
            depth--;
        }
    }

    if (options.size() > MAX_EXPANSION_SIZE) {
        result.push_back(pattern);
        return result;
    }

    result.reserve(options.size());

    for (const auto& option : options) {
        std::string combined;
        combined.reserve(prefix.size() + option.size() + suffix.size());
        combined = prefix;
        combined += option;
        combined += suffix;
        expand_and_append_results(combined, result);
    }

    return result;
}

std::vector<std::string> ExpansionEngine::expand_wildcards(const std::string& pattern) {
    std::vector<std::string> result;

    result.reserve(4);

    if (shell != nullptr && shell->get_shell_option(ShellOption::Noglob)) {
        result.push_back(pattern);
        return result;
    }

    bool has_wildcards = false;

    auto has_bracket_wildcard = [&](size_t open_index) {
        for (size_t i = open_index + 1; i < pattern.length(); ++i) {
            char c = pattern[i];
            if (c == '\x1F' && i + 1 < pattern.length()) {
                ++i;
                continue;
            }
            if (c == ']') {
                return i > open_index + 1;
            }
            if (c == '/') {
                return false;
            }
        }
        return false;
    };

    for (size_t i = 0; i < pattern.length(); ++i) {
        char c = pattern[i];

        if (c == '\x1F' && i + 1 < pattern.length()) {
            i++;
            continue;
        }

        if (c == '*' || c == '?' || (c == '[' && has_bracket_wildcard(i))) {
            has_wildcards = true;
            break;
        }
        if (config::extglob_enabled && i + 1 < pattern.length() && pattern[i + 1] == '(' &&
            (c == '+' || c == '@' || c == '!')) {
            has_wildcards = true;
            break;
        }
    }

    std::string unescaped = remove_escape_markers(pattern);

    if (!has_wildcards) {
        result.push_back(unescaped);
        return result;
    }

    bool globstar_enabled =
        shell != nullptr && shell->get_shell_option(ShellOption::Globstar) && !config::posix_mode;
    if (globstar_enabled || config::extglob_enabled) {
        ParsedGlobPattern parsed_pattern = parse_glob_pattern(unescaped);
        if ((globstar_enabled && parsed_pattern.contains_globstar) ||
            parsed_pattern.contains_extglob) {
            std::vector<std::string> globstar_matches = expand_globstar_pattern(parsed_pattern);
            if (!globstar_matches.empty()) {
                return globstar_matches;
            }
            result.push_back(unescaped);
            return result;
        }
    }

    glob_t glob_result{};

    int return_value = glob(unescaped.c_str(), GLOB_TILDE | GLOB_MARK, nullptr, &glob_result);
    if (return_value == 0) {
        result.reserve(glob_result.gl_pathc);
        for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            (void)result.emplace_back(glob_result.gl_pathv[i]);
        }
        globfree(&glob_result);
    } else if (return_value == GLOB_NOMATCH) {
        result.push_back(std::move(unescaped));
    }

    return result;
}

void ExpansionEngine::expand_and_append_results(const std::string& combined,
                                                std::vector<std::string>& result) {
    std::vector<std::string> expanded_results = expand_braces(combined);
    (void)result.insert(result.end(), std::make_move_iterator(expanded_results.begin()),
                        std::make_move_iterator(expanded_results.end()));
}

template <typename T>
void ExpansionEngine::expand_range(T start, T end, const std::string& prefix,
                                   const std::string& suffix, std::vector<std::string>& result,
                                   T stride, size_t numeric_width) {
    auto append_value = [&](T value) {
        std::string combined;
        if constexpr (std::is_same_v<T, int>) {
            combined.reserve(prefix.size() + 12 + suffix.size());
            combined = prefix;
            if (numeric_width > 0) {
                std::ostringstream formatted;
                if (value < 0) {
                    formatted << '-';
                }
                formatted << std::setw(static_cast<int>(numeric_width)) << std::setfill('0')
                          << std::abs(value);
                combined += formatted.str();
            } else {
                combined += std::to_string(value);
            }
            combined += suffix;
        } else if constexpr (std::is_same_v<T, char>) {
            combined.reserve(prefix.size() + 1 + suffix.size());
            combined = prefix;
            combined.append(1, value);
            (void)combined.append(suffix);
        }
        expand_and_append_results(combined, result);
    };

    if constexpr (std::is_same_v<T, int>) {
        const int magnitude = std::max(1, std::abs(stride));
        if (start <= end) {
            for (T i = start; i <= end;) {
                append_value(i);
                if (i > end - magnitude) {
                    break;
                }
                i += magnitude;
            }
        } else {
            for (T i = start; i >= end;) {
                append_value(i);
                if (i < end + magnitude) {
                    break;
                }
                i -= magnitude;
            }
        }
    } else if constexpr (std::is_same_v<T, char>) {
        const int magnitude = std::max(1, std::abs(static_cast<int>(stride)));
        if (start <= end) {
            for (T c = start; c <= end;) {
                append_value(c);
                if (static_cast<int>(c) > static_cast<int>(end) - magnitude) {
                    break;
                }
                c = static_cast<T>(static_cast<int>(c) + magnitude);
            }
        } else {
            for (T c = start; c >= end;) {
                append_value(c);
                if (static_cast<int>(c) < static_cast<int>(end) + magnitude) {
                    break;
                }
                c = static_cast<T>(static_cast<int>(c) - magnitude);
            }
        }
    }
}

template void ExpansionEngine::expand_range<int>(int start, int end, const std::string& prefix,
                                                 const std::string& suffix,
                                                 std::vector<std::string>& result, int stride,
                                                 size_t numeric_width);
template void ExpansionEngine::expand_range<char>(char start, char end, const std::string& prefix,
                                                  const std::string& suffix,
                                                  std::vector<std::string>& result, char stride,
                                                  size_t numeric_width);
