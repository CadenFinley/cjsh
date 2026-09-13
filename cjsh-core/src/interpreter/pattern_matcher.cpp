/*
  pattern_matcher.cpp

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

#include "pattern_matcher.h"

#include <fnmatch.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shell_env.h"

namespace {

enum class PatternNodeKind : std::uint8_t {
    Literal,
    AnyCharacter,
    AnyString,
    CharacterClass,
    ExtendedGroup
};

struct PatternNode {
    PatternNodeKind kind = PatternNodeKind::Literal;
    char value = '\0';
    std::string character_class;
    std::vector<std::vector<PatternNode>> alternatives;
};

void append_unique(std::vector<size_t>& values, size_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

class GlobPatternParser {
   public:
    explicit GlobPatternParser(const std::string& pattern, bool top_level_alternatives)
        : pattern_(pattern), top_level_alternatives_(top_level_alternatives) {
    }

    std::vector<std::vector<PatternNode>> parse() {
        return parse_alternatives(false);
    }

   private:
    const std::string& pattern_;
    bool top_level_alternatives_ = false;
    size_t position_ = 0;

    static bool is_extglob_operator(char ch) {
        return ch == '?' || ch == '*' || ch == '+' || ch == '@' || ch == '!';
    }

    std::vector<std::vector<PatternNode>> parse_alternatives(bool stop_at_close) {
        std::vector<std::vector<PatternNode>> alternatives;
        alternatives.emplace_back();

        while (position_ < pattern_.size()) {
            char ch = pattern_[position_];
            if ((ch == '|' && (stop_at_close || top_level_alternatives_)) ||
                (stop_at_close && ch == ')')) {
                if (ch == '|') {
                    ++position_;
                    alternatives.emplace_back();
                    continue;
                }
                break;
            }

            PatternNode node;
            if (ch == '\\' && position_ + 1 < pattern_.size()) {
                node.kind = PatternNodeKind::Literal;
                node.value = pattern_[position_ + 1];
                position_ += 2;
            } else if (config::extglob_enabled && is_extglob_operator(ch) &&
                       position_ + 1 < pattern_.size() && pattern_[position_ + 1] == '(') {
                node.kind = PatternNodeKind::ExtendedGroup;
                node.value = ch;
                position_ += 2;
                node.alternatives = parse_alternatives(true);
                if (position_ < pattern_.size() && pattern_[position_] == ')') {
                    ++position_;
                } else {
                    node.kind = PatternNodeKind::Literal;
                    node.value = ch;
                    node.alternatives.clear();
                }
            } else if (ch == '*') {
                node.kind = PatternNodeKind::AnyString;
                ++position_;
            } else if (ch == '?') {
                node.kind = PatternNodeKind::AnyCharacter;
                ++position_;
            } else if (ch == '[') {
                size_t close = position_ + 1;
                if (close < pattern_.size() && (pattern_[close] == '!' || pattern_[close] == '^')) {
                    ++close;
                }
                if (close < pattern_.size() && pattern_[close] == ']') {
                    ++close;
                }
                for (; close < pattern_.size(); ++close) {
                    if (pattern_[close] == '[' && close + 1 < pattern_.size() &&
                        (pattern_[close + 1] == ':' || pattern_[close + 1] == '.' ||
                         pattern_[close + 1] == '=')) {
                        const char marker = pattern_[close + 1];
                        size_t nested_close = pattern_.find(std::string{marker, ']'}, close + 2);
                        if (nested_close == std::string::npos) {
                            close = pattern_.size();
                            break;
                        }
                        close = nested_close + 1;
                        continue;
                    }
                    if (pattern_[close] == ']') {
                        break;
                    }
                }
                if (close >= pattern_.size()) {
                    close = std::string::npos;
                }
                if (close != std::string::npos) {
                    node.kind = PatternNodeKind::CharacterClass;
                    node.character_class = pattern_.substr(position_, close - position_ + 1);
                    position_ = close + 1;
                } else {
                    node.kind = PatternNodeKind::Literal;
                    node.value = ch;
                    ++position_;
                }
            } else {
                node.kind = PatternNodeKind::Literal;
                node.value = ch;
                ++position_;
            }
            alternatives.back().push_back(std::move(node));
        }

        return alternatives;
    }
};

bool character_class_matches(char character, const std::string& pattern) {
    if (pattern.size() < 3 || pattern.front() != '[' || pattern.back() != ']') {
        return false;
    }
    const std::string candidate(1, character);
    return fnmatch(pattern.c_str(), candidate.c_str(), 0) == 0;
}

bool matches_simple_sequence(const std::vector<PatternNode>& sequence, const std::string& text) {
    if (sequence.size() != text.size() &&
        std::none_of(sequence.begin(), sequence.end(), [](const PatternNode& node) {
            return node.kind == PatternNodeKind::AnyString;
        })) {
        return false;
    }
    size_t node_index = 0;
    size_t text_index = 0;
    size_t star_index = sequence.size();
    size_t star_text_index = 0;

    // Ordinary globs need only a full match, not every possible endpoint. On a
    // mismatch, extend the most recent star by one byte and retry its suffix.
    while (text_index < text.size()) {
        if (node_index < sequence.size()) {
            const auto& node = sequence[node_index];
            if (node.kind == PatternNodeKind::AnyString) {
                star_index = node_index++;
                star_text_index = text_index;
                continue;
            }
            if ((node.kind == PatternNodeKind::Literal && node.value == text[text_index]) ||
                node.kind == PatternNodeKind::AnyCharacter ||
                (node.kind == PatternNodeKind::CharacterClass &&
                 character_class_matches(text[text_index], node.character_class))) {
                ++node_index;
                ++text_index;
                continue;
            }
        }
        if (star_index == sequence.size()) {
            return false;
        }
        node_index = star_index + 1;
        text_index = ++star_text_index;
    }
    while (node_index < sequence.size() &&
           sequence[node_index].kind == PatternNodeKind::AnyString) {
        ++node_index;
    }
    return node_index == sequence.size();
}

std::vector<size_t> match_sequence(const std::vector<PatternNode>& sequence, size_t node_index,
                                   const std::string& text, size_t text_index);

std::vector<size_t> match_alternatives(const std::vector<std::vector<PatternNode>>& alternatives,
                                       const std::string& text, size_t text_index) {
    if (alternatives.size() == 1) {
        return match_sequence(alternatives.front(), 0, text, text_index);
    }
    std::vector<size_t> endpoints;
    std::vector<bool> seen(text.size() + 1, false);
    for (const auto& alternative : alternatives) {
        for (size_t endpoint : match_sequence(alternative, 0, text, text_index)) {
            if (!seen[endpoint]) {
                seen[endpoint] = true;
                endpoints.push_back(endpoint);
            }
        }
    }
    return endpoints;
}

std::vector<size_t> repeat_group(const PatternNode& node, const std::string& text,
                                 const std::vector<size_t>& initial) {
    std::vector<size_t> endpoints = initial;
    std::vector<bool> seen(text.size() + 1, false);
    for (size_t endpoint : endpoints) {
        seen[endpoint] = true;
    }
    // Visit each reachable offset once, including for overlapping or empty alternatives.
    // This worklist grows while matching, so iterators would be invalidated.
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (size_t cursor = 0; cursor < endpoints.size(); ++cursor) {
        size_t begin = endpoints[cursor];
        for (size_t endpoint : match_alternatives(node.alternatives, text, begin)) {
            if (!seen[endpoint]) {
                seen[endpoint] = true;
                endpoints.push_back(endpoint);
            }
        }
    }
    return endpoints;
}

std::vector<size_t> match_node(const PatternNode& node, const std::string& text,
                               size_t text_index) {
    switch (node.kind) {
        case PatternNodeKind::Literal:
            return text_index < text.size() && text[text_index] == node.value
                       ? std::vector<size_t>{text_index + 1}
                       : std::vector<size_t>{};
        case PatternNodeKind::AnyCharacter:
            return text_index < text.size() ? std::vector<size_t>{text_index + 1}
                                            : std::vector<size_t>{};
        case PatternNodeKind::AnyString: {
            std::vector<size_t> endpoints;
            endpoints.reserve(text.size() - text_index + 1);
            for (size_t endpoint = text_index; endpoint <= text.size(); ++endpoint) {
                endpoints.push_back(endpoint);
            }
            return endpoints;
        }
        case PatternNodeKind::CharacterClass:
            return text_index < text.size() &&
                           character_class_matches(text[text_index], node.character_class)
                       ? std::vector<size_t>{text_index + 1}
                       : std::vector<size_t>{};
        case PatternNodeKind::ExtendedGroup:
            break;
    }

    std::vector<size_t> direct = match_alternatives(node.alternatives, text, text_index);
    if (node.value == '@') {
        return direct;
    }
    if (node.value == '?') {
        append_unique(direct, text_index);
        return direct;
    }
    if (node.value == '*') {
        return repeat_group(node, text, {text_index});
    }
    if (node.value == '+') {
        return repeat_group(node, text, direct);
    }
    if (node.value == '!') {
        std::vector<bool> excluded(text.size() + 1, false);
        for (size_t endpoint : direct) {
            excluded[endpoint] = true;
        }
        std::vector<size_t> endpoints;
        for (size_t endpoint = text_index; endpoint <= text.size(); ++endpoint) {
            if (!excluded[endpoint]) {
                endpoints.push_back(endpoint);
            }
        }
        return endpoints;
    }
    return {};
}

std::vector<size_t> match_sequence(const std::vector<PatternNode>& sequence, size_t node_index,
                                   const std::string& text, size_t text_index) {
    // All paths reaching the same offset at a node have the same remaining work.
    // Advance a unique frontier instead of recursively exploring every star split.
    // Memory is bounded by input length; recursion is only needed for nested groups.
    std::vector<size_t> positions{text_index};
    std::vector<size_t> next;
    std::vector<bool> seen;
    for (; node_index < sequence.size() && !positions.empty(); ++node_index) {
        const auto& node = sequence[node_index];
        if (node.kind == PatternNodeKind::AnyString) {
            const size_t begin = *std::min_element(positions.begin(), positions.end());
            positions.clear();
            for (size_t pos = begin; pos <= text.size(); ++pos) {
                positions.push_back(pos);
            }
            continue;
        }
        if (node.kind == PatternNodeKind::ExtendedGroup && positions.size() == 1) {
            positions = match_node(node, text, positions.front());
            continue;
        }
        next.clear();
        if (node.kind == PatternNodeKind::ExtendedGroup) {
            seen.assign(text.size() + 1, false);
            for (size_t pos : positions) {
                for (size_t endpoint : match_node(node, text, pos)) {
                    if (!seen[endpoint]) {
                        seen[endpoint] = true;
                        next.push_back(endpoint);
                    }
                }
            }
        } else {
            // Literal, '?' and character-class transitions are one-to-one, so
            // unique input offsets remain unique without a membership table.
            for (size_t pos : positions) {
                if (pos < text.size() &&
                    (node.kind == PatternNodeKind::AnyCharacter ||
                     (node.kind == PatternNodeKind::Literal && node.value == text[pos]) ||
                     (node.kind == PatternNodeKind::CharacterClass &&
                      character_class_matches(text[pos], node.character_class)))) {
                    next.push_back(pos + 1);
                }
            }
        }
        positions.swap(next);
    }
    return positions;
}

struct CompiledPattern {
    std::string pattern;
    bool extglob_enabled;
    bool top_level_alternatives;
    std::vector<std::vector<PatternNode>> alternatives;
};

const CompiledPattern& compiled_pattern_for(const std::string& pattern,
                                            bool top_level_alternatives) {
    auto sanitize_quotes = [](const std::string& raw_pattern) {
        std::string cleaned;
        cleaned.reserve(raw_pattern.size());
        char quote = '\0';

        for (size_t i = 0; i < raw_pattern.size(); ++i) {
            char ch = raw_pattern[i];

            if (ch == '\\' && quote != '\'' && i + 1 < raw_pattern.size()) {
                cleaned += ch;
                cleaned += raw_pattern[i + 1];
                ++i;
                continue;
            }

            if (ch == '\'' || ch == '"') {
                if (quote == '\0') {
                    quote = ch;
                } else if (quote == ch) {
                    quote = '\0';
                } else {
                    cleaned += ch;
                }
                continue;
            }

            if (quote != '\0' && (ch == '*' || ch == '?' || ch == '[' || ch == '|' || ch == '\\')) {
                cleaned += '\\';
            }
            cleaned += ch;
        }

        return cleaned;
    };

    // Parameter replacement tests many substrings against the same pattern.
    // Keep one parsed pattern per thread, with every parsing option in the key.
    // Character classes still use the current locale when they are matched.
    thread_local std::optional<CompiledPattern> cached;
    if (!cached || cached->pattern != pattern ||
        cached->extglob_enabled != config::extglob_enabled ||
        cached->top_level_alternatives != top_level_alternatives) {
        const std::string sanitized_pattern = sanitize_quotes(pattern);
        GlobPatternParser parser(sanitized_pattern, top_level_alternatives);
        cached = CompiledPattern{pattern, config::extglob_enabled, top_level_alternatives,
                                 parser.parse()};
    }
    return *cached;
}

}  // namespace

bool PatternMatcher::matches_pattern(const std::string& text, const std::string& pattern,
                                     bool top_level_alternatives) const {
    const auto& compiled = compiled_pattern_for(pattern, top_level_alternatives);
    for (const auto& alternative : compiled.alternatives) {
        const bool has_extended_group = std::any_of(
            alternative.begin(), alternative.end(),
            [](const PatternNode& node) { return node.kind == PatternNodeKind::ExtendedGroup; });
        if (!has_extended_group) {
            if (matches_simple_sequence(alternative, text)) {
                return true;
            }
            continue;
        }
        auto endpoints = match_sequence(alternative, 0, text, 0);
        if (std::find(endpoints.begin(), endpoints.end(), text.size()) != endpoints.end()) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<size_t>> PatternMatcher::match_end_positions(const std::string& text,
                                                                       const std::string& pattern,
                                                                       bool longest) const {
    const auto& compiled = compiled_pattern_for(pattern, false);
    for (const auto& alternative : compiled.alternatives) {
        if (std::any_of(alternative.begin(), alternative.end(), [](const PatternNode& node) {
                return node.kind == PatternNodeKind::ExtendedGroup;
            })) {
            return std::nullopt;
        }
    }

    const size_t no_match = std::string::npos;
    const auto choose = [longest, no_match](size_t left, size_t right) {
        if (left == no_match) {
            return right;
        }
        if (right == no_match) {
            return left;
        }
        return longest ? std::max(left, right) : std::min(left, right);
    };
    std::vector<size_t> endpoints(text.size() + 1, no_match);
    std::vector<size_t> next(text.size() + 1);
    std::vector<size_t> current(text.size() + 1);
    for (const auto& alternative : compiled.alternatives) {
        // An empty pattern ends at its starting byte. Work backwards through the
        // pattern, sharing suffix results instead of matching every substring.
        for (size_t pos = 0; pos <= text.size(); ++pos) {
            next[pos] = pos;
        }
        for (auto node = alternative.rbegin(); node != alternative.rend(); ++node) {
            current[text.size()] =
                node->kind == PatternNodeKind::AnyString ? next[text.size()] : no_match;
            for (size_t pos = text.size(); pos > 0;) {
                --pos;
                if (node->kind == PatternNodeKind::AnyString) {
                    current[pos] = choose(next[pos], current[pos + 1]);
                } else {
                    const bool matches =
                        node->kind == PatternNodeKind::AnyCharacter ||
                        (node->kind == PatternNodeKind::Literal && node->value == text[pos]) ||
                        (node->kind == PatternNodeKind::CharacterClass &&
                         character_class_matches(text[pos], node->character_class));
                    current[pos] = matches ? next[pos + 1] : no_match;
                }
            }
            next.swap(current);
        }
        for (size_t pos = 0; pos <= text.size(); ++pos) {
            endpoints[pos] = choose(endpoints[pos], next[pos]);
        }
    }
    return endpoints;
}
