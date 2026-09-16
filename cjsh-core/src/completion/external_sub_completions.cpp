/*
  external_sub_completions.cpp

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

#include "external_sub_completions.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "builtins_completions_handler.h"
#include "cjsh_filesystem.h"
#include "completion_context.h"
#include "completion_spec.h"
#include "completion_tracker.h"
#include "completion_utils.h"
#include "exec.h"
#include "isocline.h"
#include "shell_env.h"
#include "string_utils.h"

namespace {

using builtin_completions::CommandDoc;
using builtin_completions::CompletionEntry;
using builtin_completions::EntryKind;
using completion_specs::CompletionValueSpec;
using completion_specs::ValueRequirement;
using completion_specs::ValueSeparator;
using completion_specs::ValueType;

struct OptionState {
    std::vector<std::string> names;
    CompletionValueSpec value;
    std::string description;
    bool active{false};
};

struct CommandState {
    std::string name;
    std::string description;
    bool active{false};
};

bool append_description_continuation(std::string& description, const std::string& line) {
    std::string extra = string_utils::trim_ascii_whitespace_copy(line);
    if (extra.empty()) {
        return false;
    }
    if (!description.empty()) {
        description += ' ';
    }
    description += extra;
    return true;
}

enum class Section : std::uint8_t {
    None,
    Options,
    Commands
};

std::mutex g_cache_mutex;
std::unordered_map<std::string, CommandDoc> g_memory_cache;
std::unordered_set<std::string> g_failed_targets;
std::unordered_map<std::string, std::string> g_summary_cache;
std::optional<std::string> lookup_summary_cache(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_summary_cache.find(key);
    if (it == g_summary_cache.end()) {
        return std::nullopt;
    }
    return it->second;
}

void remember_summary_cache(const std::string& key, std::string summary) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_summary_cache[key] = std::move(summary);
}

std::string collapse_whitespace(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    bool in_space = false;
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!in_space) {
                result.push_back(' ');
                in_space = true;
            }
        } else {
            result.push_back(ch);
            in_space = false;
        }
    }
    return string_utils::trim_ascii_whitespace_copy(result);
}

std::string sanitize_description(const std::string& text) {
    std::string collapsed = collapse_whitespace(text);
    if (collapsed.empty()) {
        return collapsed;
    }
    return collapsed;
}

bool attach_executable_path_if_missing(CommandDoc& doc, const std::string& doc_target) {
    if (!doc.executable_path.empty()) {
        return false;
    }

    std::string resolved_path = cjsh_filesystem::find_executable_in_path(doc_target);
    if (resolved_path.empty()) {
        return false;
    }

    doc.executable_path = std::move(resolved_path);
    return true;
}

bool has_lowercase(const std::string& value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char ch) { return std::islower(ch) != 0; });
}

std::string normalize_subcommand_token(const std::string& token) {
    if (!has_lowercase(token)) {
        return token;
    }

    std::size_t first_alpha = std::string::npos;
    for (std::size_t i = 0; i < token.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(token[i]);
        if (std::isalpha(ch) != 0) {
            first_alpha = i;
            break;
        }
    }

    if (first_alpha == std::string::npos) {
        return token;
    }

    unsigned char first_char = static_cast<unsigned char>(token[first_alpha]);
    if (std::isupper(first_char) == 0) {
        return token;
    }

    return string_utils::to_lower_copy(token);
}

bool is_section_heading(const std::string& trimmed_line) {
    if (trimmed_line.empty()) {
        return false;
    }

    bool has_alpha = false;
    for (char ch : trimmed_line) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::islower(uch) != 0) {
            return false;
        }
        if (std::isalpha(uch) != 0) {
            has_alpha = true;
        }
    }
    return has_alpha;
}

Section section_from_heading(const std::string& heading) {
    std::string upper = string_utils::to_upper_copy(heading);
    if (upper.find("OPTION") != std::string::npos) {
        return Section::Options;
    }
    if (upper.find("COMMAND") != std::string::npos ||
        upper.find("SUBCOMMAND") != std::string::npos) {
        return Section::Commands;
    }
    return Section::None;
}

bool is_token_allowed_for_combination(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    if (token[0] == '-' || token[0] == '~') {
        return false;
    }
    if (token.find('/') != std::string::npos) {
        return false;
    }
    if (token.find('.') != std::string::npos) {
        return false;
    }

    bool has_alpha = false;
    for (char ch : token) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalpha(uch) != 0) {
            has_alpha = true;
        }
        if ((std::isalnum(uch) == 0) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return has_alpha;
}

std::pair<std::string, std::string> split_option_line(const std::string& line) {
    std::size_t double_space = line.find("  ");
    std::size_t tab_pos = line.find('\t');
    std::size_t split_pos = std::string::npos;

    if (double_space != std::string::npos) {
        split_pos = double_space;
    }
    if (tab_pos != std::string::npos && (split_pos == std::string::npos || tab_pos < split_pos)) {
        split_pos = tab_pos;
    }
    if (split_pos == std::string::npos) {
        std::size_t first_space = line.find(' ');
        if (first_space != std::string::npos) {
            split_pos = line.find_first_not_of(' ', first_space);
        }
    }

    if (split_pos == std::string::npos) {
        return {string_utils::trim_right_ascii_whitespace_copy(line), std::string{}};
    }

    std::string name_part =
        string_utils::trim_right_ascii_whitespace_copy(line.substr(0, split_pos));
    std::string desc_part = string_utils::trim_ascii_whitespace_copy(line.substr(split_pos));
    return {name_part, desc_part};
}

struct ParsedOptionSpec {
    std::vector<std::string> names;
    CompletionValueSpec value;
};

ValueType infer_value_type(const std::string& value_name, const std::vector<std::string>& choices) {
    if (!choices.empty()) {
        return ValueType::Enum;
    }

    std::string upper = string_utils::to_upper_copy(value_name);
    if (upper.find("DIRECTORY") != std::string::npos || upper == "DIR") {
        return ValueType::Directory;
    }
    if (upper.find("FILE") != std::string::npos || upper.find("PATH") != std::string::npos) {
        return ValueType::File;
    }
    if (upper.find("COMMAND") != std::string::npos || upper == "CMD") {
        return ValueType::Command;
    }
    if (upper.find("BRANCH") != std::string::npos || upper == "REF") {
        return ValueType::Branch;
    }
    if (upper.find("PROCESS") != std::string::npos || upper == "PID") {
        return ValueType::Process;
    }
    return value_name.empty() ? ValueType::None : ValueType::Text;
}

std::vector<std::string> parse_value_choices(const std::string& value_expression) {
    std::string body = string_utils::trim_ascii_whitespace_copy(value_expression);
    if (body.size() >= 2 &&
        ((body.front() == '{' && body.back() == '}') ||
         (body.front() == '(' && body.back() == ')' && body.find('|') != std::string::npos))) {
        body = body.substr(1, body.size() - 2);
    } else {
        return {};
    }

    const char delimiter = body.find('|') != std::string::npos ? '|' : ',';
    std::vector<std::string> choices;
    std::string current;
    for (char ch : body) {
        if (ch == delimiter) {
            std::string choice = string_utils::trim_ascii_whitespace_copy(current);
            if (!choice.empty()) {
                choices.push_back(std::move(choice));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    std::string choice = string_utils::trim_ascii_whitespace_copy(current);
    if (!choice.empty()) {
        choices.push_back(std::move(choice));
    }
    return choices;
}

void merge_value_spec(CompletionValueSpec& destination, const CompletionValueSpec& source) {
    const bool destination_had_value = destination.requirement != ValueRequirement::None;
    if (source.requirement == ValueRequirement::Required ||
        (source.requirement == ValueRequirement::Optional &&
         destination.requirement == ValueRequirement::None)) {
        destination.requirement = source.requirement;
    }
    if (destination.type == ValueType::None || source.type == ValueType::Enum) {
        destination.type = source.type;
    }
    if (destination.name.empty()) {
        destination.name = source.name;
    }
    if (destination.choices.empty()) {
        destination.choices = source.choices;
    }
    if (destination_had_value && destination.separator != source.separator) {
        destination.separator = ValueSeparator::Either;
    } else if (source.requirement != ValueRequirement::None) {
        destination.separator = source.separator;
    }
}

ParsedOptionSpec parse_option_spec(const std::string& spec) {
    std::vector<std::string> variants;
    std::string current;
    int nesting = 0;
    for (char ch : spec) {
        if (ch == '[' || ch == '{' || ch == '(' || ch == '<') {
            ++nesting;
        } else if ((ch == ']' || ch == '}' || ch == ')' || ch == '>') && nesting > 0) {
            --nesting;
        }

        if (ch == ',' && nesting == 0) {
            std::string cleaned = string_utils::trim_ascii_whitespace_copy(current);
            if (!cleaned.empty()) {
                variants.push_back(cleaned);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        std::string cleaned = string_utils::trim_ascii_whitespace_copy(current);
        if (!cleaned.empty()) {
            variants.push_back(cleaned);
        }
    }

    ParsedOptionSpec result;
    result.names.reserve(variants.size());
    for (const std::string& variant : variants) {
        if (variant.empty() || variant[0] != '-') {
            continue;
        }

        std::size_t name_end = variant.find_first_of(" \t=[{(<");
        std::string name = variant.substr(0, name_end);
        while (!name.empty() && (name.back() == ',' || name.back() == ';' || name.back() == '.')) {
            name.pop_back();
        }
        if (name.empty() || name[0] != '-') {
            continue;
        }
        if (std::find(result.names.begin(), result.names.end(), name) == result.names.end()) {
            result.names.push_back(name);
        }

        if (name_end == std::string::npos) {
            continue;
        }

        std::string value_expression =
            string_utils::trim_ascii_whitespace_copy(variant.substr(name_end));
        if (value_expression.empty()) {
            continue;
        }

        CompletionValueSpec value;
        value.requirement = ValueRequirement::Required;
        value.separator = ValueSeparator::Space;

        if (value_expression.front() == '[' && value_expression.back() == ']') {
            value.requirement = ValueRequirement::Optional;
            value_expression = value_expression.substr(1, value_expression.size() - 2);
        }
        value_expression = string_utils::trim_ascii_whitespace_copy(value_expression);
        if (!value_expression.empty() && value_expression.front() == '=') {
            value.separator = ValueSeparator::Equals;
            value_expression.erase(value_expression.begin());
        } else if (!value_expression.empty() &&
                   (value_expression.front() == '<' || value_expression.front() == '{' ||
                    value_expression.front() == '(')) {
            value.separator = ValueSeparator::Either;
        }

        value_expression = string_utils::trim_ascii_whitespace_copy(value_expression);
        value.choices = parse_value_choices(value_expression);
        if (!value.choices.empty()) {
            value.name = "VALUE";
        } else {
            value.name = value_expression;
            if (value.name.size() >= 2 &&
                ((value.name.front() == '<' && value.name.back() == '>') ||
                 (value.name.front() == '{' && value.name.back() == '}') ||
                 (value.name.front() == '(' && value.name.back() == ')'))) {
                value.name = value.name.substr(1, value.name.size() - 2);
            }
        }
        value.type = infer_value_type(value.name, value.choices);
        merge_value_spec(result.value, value);
    }

    if (result.names.empty() && !spec.empty() && spec[0] == '-') {
        std::string fallback = spec;
        std::size_t space_pos = fallback.find_first_of(" \t");
        if (space_pos != std::string::npos) {
            fallback = fallback.substr(0, space_pos);
        }
        std::size_t bracket_pos = fallback.find_first_of("[{(");
        if (bracket_pos != std::string::npos) {
            fallback = fallback.substr(0, bracket_pos);
        }
        while (!fallback.empty() &&
               (fallback.back() == ',' || fallback.back() == ';' || fallback.back() == '.')) {
            fallback.pop_back();
        }
        if (!fallback.empty() && fallback[0] == '-') {
            result.names.push_back(fallback);
        }
    }

    return result;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    lines.reserve(512);
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(current);
    return lines;
}

std::string sanitize_man_output(const std::string& raw) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\b') {
            if (!cleaned.empty()) {
                cleaned.pop_back();
            }
            continue;
        }
        cleaned.push_back(ch);
    }
    return cleaned;
}

std::vector<std::string> build_prefixes(const std::string& doc_target) {
    std::vector<std::string> prefixes;
    prefixes.push_back(doc_target);

    std::string spaced = doc_target;
    std::replace(spaced.begin(), spaced.end(), '-', ' ');
    if (std::find(prefixes.begin(), prefixes.end(), spaced) == prefixes.end()) {
        prefixes.push_back(spaced);
    }

    std::size_t dash_pos = doc_target.find('-');
    if (dash_pos != std::string::npos) {
        std::string base = doc_target.substr(0, dash_pos);
        if (std::find(prefixes.begin(), prefixes.end(), base) == prefixes.end()) {
            prefixes.push_back(base);
        }
    }

    std::sort(prefixes.begin(), prefixes.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    return prefixes;
}

std::string strip_known_prefix(const std::string& line, const std::vector<std::string>& prefixes) {
    for (const auto& prefix : prefixes) {
        if (prefix.empty()) {
            continue;
        }

        const std::string variants[] = {prefix + " ", prefix + "\t", prefix + "-",
                                        prefix + "::", prefix + ":"};
        for (const auto& variant : variants) {
            if (line.rfind(variant, 0) == 0) {
                return line.substr(variant.size());
            }
        }
    }
    return line;
}

std::optional<std::pair<std::string, std::string>> parse_command_line(
    const std::vector<std::string>& prefixes, const std::string& original_line) {
    std::string working = string_utils::trim_ascii_whitespace_copy(original_line);
    if (working.empty()) {
        return std::nullopt;
    }

    working = strip_known_prefix(working, prefixes);

    if (working.empty()) {
        return std::nullopt;
    }

    if (working[0] == '-' || working[0] == '*') {
        std::size_t pos = working.find_first_not_of("-* ");
        if (pos != std::string::npos) {
            working = working.substr(pos);
        } else {
            return std::nullopt;
        }
    }

    if (working.empty() || !has_lowercase(working)) {
        return std::nullopt;
    }

    std::size_t first_space = working.find_first_of(" \t");
    std::size_t split_pos = working.find("  ");
    std::size_t tab_pos = working.find('\t');
    if (tab_pos != std::string::npos && (split_pos == std::string::npos || tab_pos < split_pos)) {
        split_pos = tab_pos;
    }

    std::string name_part;
    std::string description_part;

    if (split_pos != std::string::npos) {
        name_part = string_utils::trim_ascii_whitespace_copy(working.substr(0, split_pos));
        description_part = string_utils::trim_ascii_whitespace_copy(working.substr(split_pos));
    } else {
        std::size_t hyphen_pos = working.find(" - ");
        if (hyphen_pos != std::string::npos && hyphen_pos != 0 &&
            first_space != std::string::npos && hyphen_pos == first_space + 1) {
            name_part = string_utils::trim_ascii_whitespace_copy(working.substr(0, hyphen_pos));
            description_part =
                string_utils::trim_ascii_whitespace_copy(working.substr(hyphen_pos + 3));
        } else {
            std::size_t double_colon_pos = working.find("::");
            if (double_colon_pos != std::string::npos && double_colon_pos != 0 &&
                (first_space == std::string::npos || double_colon_pos < first_space)) {
                name_part =
                    string_utils::trim_ascii_whitespace_copy(working.substr(0, double_colon_pos));
                description_part =
                    string_utils::trim_ascii_whitespace_copy(working.substr(double_colon_pos + 2));
            } else {
                std::size_t paren_open = working.find('(');
                if (paren_open != std::string::npos &&
                    (first_space == std::string::npos || paren_open < first_space)) {
                    std::size_t paren_close = working.find(')', paren_open + 1);
                    if (paren_close == std::string::npos) {
                        return std::nullopt;
                    }
                    if (paren_close == paren_open + 1) {
                        return std::nullopt;
                    }

                    unsigned char first_inside =
                        static_cast<unsigned char>(working[paren_open + 1]);
                    if (std::isdigit(first_inside) == 0) {
                        return std::nullopt;
                    }
                    for (std::size_t idx = paren_open + 2; idx < paren_close; ++idx) {
                        unsigned char ch = static_cast<unsigned char>(working[idx]);
                        if (std::isalnum(ch) == 0 && ch != '-') {
                            return std::nullopt;
                        }
                    }

                    name_part = string_utils::trim_ascii_whitespace_copy(
                        working.substr(0, paren_close + 1));
                    std::size_t desc_start = working.find_first_not_of(" \t", paren_close + 1);
                    if (desc_start != std::string::npos) {
                        description_part =
                            string_utils::trim_ascii_whitespace_copy(working.substr(desc_start));
                    } else {
                        description_part.clear();
                    }
                } else {
                    std::string candidate_name;
                    std::string candidate_description;

                    if (first_space == std::string::npos) {
                        candidate_name = working;
                        candidate_description.clear();
                    } else {
                        candidate_name = string_utils::trim_ascii_whitespace_copy(
                            working.substr(0, first_space));
                        candidate_description =
                            string_utils::trim_ascii_whitespace_copy(working.substr(first_space));
                    }

                    auto is_simple_command_name = [](const std::string& name) {
                        if (name.empty()) {
                            return false;
                        }
                        unsigned char first_char = static_cast<unsigned char>(name[0]);
                        if (std::islower(first_char) == 0 && first_char != '_') {
                            return false;
                        }
                        for (char ch : name) {
                            unsigned char uch = static_cast<unsigned char>(ch);
                            if (std::isalpha(uch) != 0 && std::islower(uch) == 0) {
                                return false;
                            }
                            if (std::isalnum(uch) == 0 && ch != '-' && ch != '_') {
                                return false;
                            }
                        }
                        return has_lowercase(name);
                    };

                    if (!is_simple_command_name(candidate_name)) {
                        return std::nullopt;
                    }

                    name_part = candidate_name;
                    description_part = candidate_description;
                }
            }
        }
    }

    if (name_part.empty()) {
        return std::nullopt;
    }

    std::size_t special_pos = name_part.find_first_of(" \t([{:");
    if (special_pos != std::string::npos) {
        name_part = name_part.substr(0, special_pos);
    }

    while (!name_part.empty() &&
           (name_part.back() == ':' || name_part.back() == ';' || name_part.back() == ',')) {
        name_part.pop_back();
    }

    if (name_part.empty()) {
        return std::nullopt;
    }

    if (!std::isalpha(static_cast<unsigned char>(name_part[0])) && name_part[0] != '_') {
        return std::nullopt;
    }

    if (!has_lowercase(name_part)) {
        return std::nullopt;
    }

    name_part = normalize_subcommand_token(name_part);

    return std::make_pair(name_part, description_part);
}

void flush_option_state(OptionState& state, std::vector<CompletionEntry>& entries,
                        std::unordered_set<std::string>& seen) {
    if (!state.active || state.names.empty()) {
        return;
    }

    std::string description = sanitize_description(state.description);
    if (description.empty()) {
        description = "option";
    }

    std::vector<std::string> unique_names;
    unique_names.reserve(state.names.size());
    for (const auto& name : state.names) {
        if (!name.empty() && seen.insert("O|" + name).second) {
            unique_names.push_back(name);
        }
    }
    if (unique_names.empty()) {
        state = OptionState{};
        return;
    }

    auto primary_it = std::max_element(unique_names.begin(), unique_names.end(),
                                       [](const std::string& lhs, const std::string& rhs) {
                                           const bool lhs_long = lhs.rfind("--", 0) == 0;
                                           const bool rhs_long = rhs.rfind("--", 0) == 0;
                                           if (lhs_long != rhs_long) {
                                               return !lhs_long;
                                           }
                                           return lhs.size() < rhs.size();
                                       });

    CompletionEntry entry;
    entry.text = primary_it == unique_names.end() ? unique_names.front() : *primary_it;
    entry.description = std::move(description);
    entry.kind = EntryKind::Option;
    entry.value = std::move(state.value);
    entry.repeatable = true;  // Man pages generally do not describe repeatability reliably.

    for (const auto& name : unique_names) {
        if (name != entry.text) {
            entry.aliases.push_back(name);
        }
    }
    entries.push_back(std::move(entry));

    state = OptionState{};
}

void flush_command_state(CommandState& state, std::vector<CompletionEntry>& entries,
                         std::unordered_set<std::string>& seen) {
    if (!state.active || state.name.empty()) {
        return;
    }

    std::string description = sanitize_description(state.description);
    if (description.empty()) {
        description = "subcommand";
    }

    std::string key = "S|" + state.name;
    if (seen.insert(key).second) {
        entries.push_back({state.name, description, EntryKind::Subcommand});
    }

    state = CommandState{};
}

std::vector<CompletionEntry> parse_man_text(const std::string& doc_target,
                                            const std::string& man_text) {
    std::vector<CompletionEntry> entries;
    entries.reserve(64);

    std::unordered_set<std::string> seen;
    OptionState option_state;
    CommandState command_state;
    Section section = Section::None;

    std::vector<std::string> prefixes = build_prefixes(doc_target);

    for (const std::string& raw_line : split_lines(man_text)) {
        std::string trimmed_line = string_utils::trim_ascii_whitespace_copy(raw_line);

        if (trimmed_line.empty()) {
            flush_option_state(option_state, entries, seen);
            flush_command_state(command_state, entries, seen);
            continue;
        }

        if (is_section_heading(trimmed_line)) {
            flush_option_state(option_state, entries, seen);
            flush_command_state(command_state, entries, seen);
            section = section_from_heading(trimmed_line);
            continue;
        }

        std::string left_trimmed = string_utils::trim_left_ascii_whitespace_copy(raw_line);

        if (section == Section::Options ||
            (!left_trimmed.empty() && left_trimmed[0] == '-' && section == Section::None)) {
            if (!left_trimmed.empty() && left_trimmed[0] == '-') {
                flush_option_state(option_state, entries, seen);

                auto split = split_option_line(left_trimmed);
                auto parsed = parse_option_spec(split.first);
                if (!parsed.names.empty()) {
                    option_state.names = std::move(parsed.names);
                    option_state.value = std::move(parsed.value);
                    option_state.description = split.second;
                    option_state.active = true;
                    continue;
                }
            } else if (option_state.active) {
                (void)append_description_continuation(option_state.description, left_trimmed);
                continue;
            }
        }

        if ((section == Section::Options && option_state.active) &&
            append_description_continuation(option_state.description, left_trimmed)) {
            continue;
        }

        if (section == Section::Commands) {
            auto parsed = parse_command_line(prefixes, left_trimmed);
            if (parsed.has_value()) {
                flush_command_state(command_state, entries, seen);
                command_state.name = parsed->first;
                command_state.description = parsed->second;
                command_state.active = true;
                continue;
            } else if (command_state.active) {
                std::string extra = string_utils::trim_ascii_whitespace_copy(left_trimmed);
                if (!extra.empty()) {
                    if (!command_state.description.empty()) {
                        command_state.description += ' ';
                    }
                    command_state.description += extra;
                }
                continue;
            }
        }

        if (option_state.active) {
            flush_option_state(option_state, entries, seen);
        }
        if (command_state.active) {
            flush_command_state(command_state, entries, seen);
        }
    }

    flush_option_state(option_state, entries, seen);
    flush_command_state(command_state, entries, seen);

    return entries;
}

std::string strip_summary_prefix(const std::string& doc_target, const std::string& line) {
    std::string working = string_utils::trim_ascii_whitespace_copy(line);
    if (working.empty()) {
        return working;
    }

    const std::string patterns[] = {" - ", " \\- ", " \xE2\x80\x94 ", " \xE2\x80\x93 ", " -- "};
    for (const auto& pattern : patterns) {
        std::size_t pos = working.find(pattern);
        if (pos != std::string::npos && pos + pattern.size() < working.size()) {
            std::string candidate = working.substr(pos + pattern.size());
            return sanitize_description(candidate);
        }
    }

    std::size_t hyphen_pos = working.find('-');
    if (hyphen_pos != std::string::npos && hyphen_pos + 1 < working.size()) {
        std::string candidate = working.substr(hyphen_pos + 1);
        while (!candidate.empty() && (candidate.front() == '-' || candidate.front() == ' ' ||
                                      candidate.front() == '\t')) {
            (void)candidate.erase(candidate.begin());
        }
        candidate = string_utils::trim_ascii_whitespace_copy(candidate);
        if (!candidate.empty()) {
            return sanitize_description(candidate);
        }
    }

    if (!doc_target.empty() && working.rfind(doc_target, 0) == 0) {
        std::string candidate =
            string_utils::trim_ascii_whitespace_copy(working.substr(doc_target.size()));
        if (!candidate.empty() && candidate.front() == '-') {
            (void)candidate.erase(candidate.begin());
            candidate = string_utils::trim_ascii_whitespace_copy(candidate);
        }
        if (!candidate.empty()) {
            return sanitize_description(candidate);
        }
    }

    return sanitize_description(working);
}

std::string extract_command_summary(const std::string& doc_target, const std::string& man_text) {
    if (man_text.empty()) {
        return {};
    }

    std::vector<std::string> lines = split_lines(man_text);
    bool in_name_section = false;
    std::string collected_line;

    for (const auto& raw_line : lines) {
        std::string trimmed_line = string_utils::trim_ascii_whitespace_copy(raw_line);

        if (!in_name_section) {
            if (trimmed_line.empty()) {
                continue;
            }
            std::string upper = string_utils::to_upper_copy(trimmed_line);
            if (upper == "NAME") {
                in_name_section = true;
            }
            continue;
        }

        if (trimmed_line.empty()) {
            if (!collected_line.empty()) {
                break;
            }
            continue;
        }

        if (is_section_heading(trimmed_line)) {
            if (!collected_line.empty()) {
                break;
            }
            continue;
        }

        if (collected_line.empty()) {
            collected_line = trimmed_line;
        } else {
            collected_line += ' ' + trimmed_line;
        }

        break;
    }

    if (collected_line.empty()) {
        return {};
    }

    return strip_summary_prefix(doc_target, collected_line);
}

std::string normalize_key(const std::string& value) {
    return string_utils::to_lower_copy(value);
}

std::string sanitize_command_for_cache(const std::string& command) {
    std::string sanitized;
    sanitized.reserve(command.size());
    for (char ch : command) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0 || ch == '-' || ch == '_') {
            sanitized.push_back(static_cast<char>(std::tolower(uch)));
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) {
        sanitized = "command";
    }
    return sanitized;
}

std::optional<CommandDoc> read_cache_entries(const std::filesystem::path& path,
                                             const std::string& doc_target) {
    auto result = cjsh_filesystem::read_file_content(path.string());
    if (result.is_error()) {
        return std::nullopt;
    }
    return completion_specs::parse_command_doc(doc_target, result.value());
}

void write_cache_entries(const std::filesystem::path& path, const std::string& doc_target,
                         const CommandDoc& doc) {
    std::error_code dir_error;
    (void)std::filesystem::create_directories(path.parent_path(), dir_error);
    if (dir_error) {
        return;
    }

    (void)cjsh_filesystem::write_file_content(
        path.string(), completion_specs::serialize_command_doc(doc_target, doc));
}

std::string fetch_man_page_text(const std::string& target) {
    std::vector<std::vector<std::string>> attempts;
    if (cjsh_env::shell_variable_is_set("CJSH_MAN_PATH")) {
        std::string env_path = cjsh_env::get_shell_variable_value("CJSH_MAN_PATH");
        std::filesystem::path man_path = cjsh_filesystem::normalize_override_path(env_path);
        std::error_code ec;
        if (man_path.empty() || !std::filesystem::exists(man_path, ec)) {
            return {};
        }
        std::string man_path_string = man_path.string();
        attempts = {{man_path_string, "-P", "cat", target}, {man_path_string, target}};
    } else if (config::secure_mode) {
        return {};
    } else {
        attempts = {{"man", "-P", "cat", target}, {"man", target}};
    }

    for (const auto& args : attempts) {
        auto output = exec_utils::execute_command_vector_for_output(args);
        if (output.success && !output.output.empty()) {
            return sanitize_man_output(output.output);
        }
    }

    return {};
}

CommandDoc load_entries_for_target(const std::string& doc_target, bool allow_fetch,
                                   bool attach_executable_path = false,
                                   bool update_memory_cache = true) {
    if (doc_target.empty()) {
        return {};
    }

    std::string key = normalize_key(doc_target);
    std::filesystem::path cache_path = cjsh_filesystem::g_cjsh_generated_completions_path() /
                                       (sanitize_command_for_cache(doc_target) + ".txt");

    auto update_cache_maps = [&](const CommandDoc& doc, bool should_update_memory_cache = true) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (should_update_memory_cache) {
            g_memory_cache[key] = doc;
        }
        if (doc.entries.empty() && doc.summary.empty()) {
            (void)g_failed_targets.insert(key);
        } else {
            (void)g_failed_targets.erase(key);
        }
    };

    if (auto registered_doc = completion_specs::lookup_registered_command_doc(doc_target);
        registered_doc.has_value()) {
        CommandDoc doc = std::move(*registered_doc);
        doc.summary_present = !doc.summary.empty();
        update_cache_maps(doc, false);
        return doc;
    }

    std::optional<CommandDoc> memoized_doc;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto memo_it = g_memory_cache.find(key);
        if (memo_it != g_memory_cache.end()) {
            memoized_doc = memo_it->second;
        }
    }
    if (memoized_doc.has_value()) {
        CommandDoc doc = std::move(*memoized_doc);
        bool path_added = false;
        if (attach_executable_path && doc.executable_path.empty()) {
            path_added = attach_executable_path_if_missing(doc, doc_target);
            if (path_added) {
                update_cache_maps(doc);
            }
        }
        if (path_added) {
            std::error_code exists_error;
            if (std::filesystem::exists(cache_path, exists_error) && !exists_error) {
                write_cache_entries(cache_path, doc_target, doc);
            }
        }
        return doc;
    }

    if (const auto* builtin_doc = builtin_completions::lookup_builtin_command_doc(doc_target)) {
        CommandDoc doc = *builtin_doc;
        doc.summary_present = !doc.summary.empty();
        update_cache_maps(doc, update_memory_cache);
        return doc;
    }

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_failed_targets.find(key) != g_failed_targets.end()) {
            return {};
        }
    }

    if (auto cached_doc_opt = read_cache_entries(cache_path, doc_target);
        cached_doc_opt.has_value()) {
        CommandDoc cached_doc = std::move(*cached_doc_opt);
        cached_doc.summary_present = true;
        if ((attach_executable_path && cached_doc.executable_path.empty()) &&
            attach_executable_path_if_missing(cached_doc, doc_target)) {
            write_cache_entries(cache_path, doc_target, cached_doc);
        }

        update_cache_maps(cached_doc, update_memory_cache);
        return cached_doc;
    }

    if (!allow_fetch) {
        return {};
    }

    std::string man_text = fetch_man_page_text(doc_target);
    CommandDoc doc;
    if (!man_text.empty()) {
        doc = parse_man_page_completion_spec(doc_target, man_text);
    } else {
        doc.summary_present = true;
    }

    if (attach_executable_path) {
        (void)attach_executable_path_if_missing(doc, doc_target);
    }

    write_cache_entries(cache_path, doc_target, doc);

    update_cache_maps(doc, update_memory_cache);
    return doc;
}

struct ResolvedCompletionContext {
    std::vector<CompletionEntry> entries;
    std::optional<CompletionEntry> pending_value;
    std::unordered_set<std::string> used_names;
    std::unordered_set<std::string> used_conflicts;
    std::vector<std::string> command_path;
    std::size_t positional_count{0};
    bool options_enabled{true};
};

const CompletionEntry* find_entry(const std::vector<CompletionEntry>& entries, EntryKind kind,
                                  const std::string& token) {
    auto it = std::find_if(entries.begin(), entries.end(), [&](const CompletionEntry& entry) {
        if (entry.kind != kind) {
            return false;
        }
        const auto names = completion_specs::entry_names(entry);
        return std::any_of(names.begin(), names.end(), [&](const auto& name) {
            return completion_utils::equals_completion_token(name, token);
        });
    });
    return it == entries.end() ? nullptr : &*it;
}

void remember_used_entry(ResolvedCompletionContext& context, const CompletionEntry& entry) {
    for (const auto& name : completion_specs::entry_names(entry)) {
        context.used_names.insert(normalize_key(name));
    }
    for (const auto& conflict : entry.conflicts) {
        context.used_conflicts.insert(normalize_key(conflict));
    }
}

ResolvedCompletionContext resolve_completion_context(const std::vector<std::string>& tokens,
                                                     std::size_t stable_count, bool allow_fetch,
                                                     bool attach_executable_path) {
    ResolvedCompletionContext context;
    if (tokens.empty()) {
        return context;
    }

    std::string current_doc = tokens[0];
    CommandDoc current_doc_data =
        load_entries_for_target(current_doc, allow_fetch, attach_executable_path);
    context.entries = current_doc_data.entries;
    context.command_path.push_back(tokens[0]);

    std::size_t max_depth = std::min(stable_count, tokens.size());
    for (std::size_t index = 1; index < max_depth; ++index) {
        const std::string& token = tokens[index];

        if (context.pending_value.has_value()) {
            context.pending_value.reset();
            continue;
        }

        if (token == "--") {
            context.options_enabled = false;
            continue;
        }

        std::string option_token = token;
        bool has_inline_value = false;
        std::size_t equals = token.find('=');
        if (equals != std::string::npos && equals != 0) {
            option_token = token.substr(0, equals);
            has_inline_value = true;
        }

        if (context.options_enabled && !option_token.empty() && option_token.front() == '-') {
            const CompletionEntry* option =
                find_entry(context.entries, EntryKind::Option, option_token);
            if (option != nullptr) {
                remember_used_entry(context, *option);
                if (!has_inline_value && option->value.requirement == ValueRequirement::Required) {
                    context.pending_value = *option;
                } else if (!has_inline_value &&
                           option->value.requirement == ValueRequirement::Optional &&
                           option->value.separator != ValueSeparator::Equals &&
                           index + 1 == max_depth) {
                    context.pending_value = *option;
                }
            }
            continue;
        }

        const CompletionEntry* subcommand =
            context.options_enabled ? find_entry(context.entries, EntryKind::Subcommand, token)
                                    : nullptr;
        if (subcommand != nullptr) {
            remember_used_entry(context, *subcommand);
            context.command_path.push_back(subcommand->text);
            current_doc.push_back('-');
            current_doc += subcommand->text;

            if (!subcommand->children.empty()) {
                context.entries = subcommand->children;
            } else {
                CommandDoc nested_doc = load_entries_for_target(current_doc, allow_fetch);
                context.entries = std::move(nested_doc.entries);
            }
            context.pending_value.reset();
            context.positional_count = 0;
            context.options_enabled = true;
            continue;
        }

        ++context.positional_count;
    }

    return context;
}

bool constraint_is_satisfied(const std::unordered_set<std::string>& used_names,
                             const std::string& name) {
    return used_names.find(normalize_key(name)) != used_names.end();
}

bool entry_is_available(const CompletionEntry& entry, const ResolvedCompletionContext& context) {
    if (!entry.repeatable) {
        for (const auto& name : completion_specs::entry_names(entry)) {
            if (constraint_is_satisfied(context.used_names, name)) {
                return false;
            }
        }
    }
    for (const auto& name : completion_specs::entry_names(entry)) {
        if (constraint_is_satisfied(context.used_conflicts, name)) {
            return false;
        }
    }
    for (const auto& conflict : entry.conflicts) {
        if (constraint_is_satisfied(context.used_names, conflict)) {
            return false;
        }
    }
    return std::all_of(entry.dependencies.begin(), entry.dependencies.end(),
                       [&](const auto& dependency) {
                           return constraint_is_satisfied(context.used_names, dependency);
                       });
}

const CompletionEntry* positional_entry_for_index(const std::vector<CompletionEntry>& entries,
                                                  std::size_t index) {
    const CompletionEntry* variadic_fallback = nullptr;
    std::size_t inferred_index = 0;
    for (const auto& entry : entries) {
        if (entry.kind != EntryKind::Positional) {
            continue;
        }
        ++inferred_index;
        std::size_t declared_index =
            entry.positional_index == 0 ? inferred_index : entry.positional_index;
        if (declared_index == index) {
            return &entry;
        }
        if (entry.variadic && declared_index <= index) {
            variadic_fallback = &entry;
        }
    }
    return variadic_fallback;
}

std::vector<completion_specs::DynamicCompletionCandidate> collect_value_candidates(
    const CompletionEntry& entry, const std::vector<std::string>& tokens,
    const ResolvedCompletionContext& context, const std::string& current_value,
    bool allow_dynamic) {
    std::vector<completion_specs::DynamicCompletionCandidate> candidates;
    candidates.reserve(entry.value.choices.size());
    for (const auto& choice : entry.value.choices) {
        candidates.push_back({choice, entry.description});
    }

    if (!allow_dynamic) {
        return candidates;
    }

    std::string provider = entry.value.dynamic_provider;
    if (provider.empty()) {
        provider = completion_specs::default_provider_for_value_type(entry.value.type);
    }
    if (provider.empty()) {
        return candidates;
    }

    completion_specs::DynamicCompletionRequest request;
    request.command = tokens.empty() ? std::string{} : tokens.front();
    request.command_path = context.command_path;
    if (tokens.size() > 1) {
        request.arguments.assign(tokens.begin() + 1, tokens.end());
    }
    request.argument_index = context.positional_count + 1;
    request.current_value = current_value;
    std::error_code cwd_error;
    request.working_directory = std::filesystem::current_path(cwd_error).string();
    request.value = entry.value;

    auto dynamic = completion_specs::request_dynamic_completions(provider, request);
    candidates.insert(candidates.end(), std::make_move_iterator(dynamic.begin()),
                      std::make_move_iterator(dynamic.end()));
    return candidates;
}

}  // namespace

completion_specs::CommandDoc parse_man_page_completion_spec(const std::string& command,
                                                            const std::string& man_text) {
    completion_specs::CommandDoc doc;
    std::string cleaned = sanitize_man_output(man_text);
    doc.entries = parse_man_text(command, cleaned);
    doc.summary = extract_command_summary(command, cleaned);
    doc.summary_present = true;
    return doc;
}

std::string get_command_summary(const std::string& command, bool allow_fetch) {
    if (command.empty()) {
        return {};
    }

    std::string key = normalize_key(command);
    if (auto cached = lookup_summary_cache(key); cached.has_value()) {
        return *cached;
    }

    CommandDoc doc = load_entries_for_target(command, allow_fetch, true, false);
    std::string summary;
    if (!doc.summary.empty()) {
        summary = doc.summary;
    } else if (!doc.executable_path.empty()) {
        summary = doc.executable_path;
    }

    // A cache-only hint must not prevent a later Tab completion from fetching
    // documentation that has not been loaded yet.
    if (allow_fetch || !summary.empty()) {
        remember_summary_cache(key, summary);
    }
    return summary;
}

bool regenerate_external_completion_cache(
    const std::string& command, bool force_refresh, bool include_subcommands,
    const ::CompletionCacheProgressCallback& progress_callback,
    const ::CompletionCacheCancelCallback& cancel_callback) {
    if (command.empty()) {
        return false;
    }

    std::vector<std::string> pending_targets = {command};
    std::unordered_set<std::string> visited_targets;
    bool root_generated = false;

    while (!pending_targets.empty()) {
        if (cancel_callback && cancel_callback()) {
            break;
        }

        std::string current_target = pending_targets.back();
        pending_targets.pop_back();

        if (current_target.empty()) {
            continue;
        }

        std::string normalized_target = normalize_key(current_target);
        if (!visited_targets.insert(normalized_target).second) {
            continue;
        }

        CompletionCacheTargetResult result = regenerate_external_completion_cache_target(
            current_target, force_refresh, include_subcommands);
        bool current_generated = result.generated;
        if (current_target == command) {
            root_generated = current_generated;
        }

        if (progress_callback) {
            progress_callback(current_target, current_generated, current_target == command);
        }

        if (cancel_callback && cancel_callback()) {
            break;
        }

        for (auto& discovered_target : result.discovered_targets) {
            pending_targets.push_back(std::move(discovered_target));
        }
    }

    return root_generated;
}

CompletionCacheTargetResult regenerate_external_completion_cache_target(const std::string& target,
                                                                        bool force_refresh,
                                                                        bool discover_subcommands) {
    CompletionCacheTargetResult result;
    if (target.empty()) {
        return result;
    }

    std::string normalized_target = normalize_key(target);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        (void)g_memory_cache.erase(normalized_target);
        (void)g_failed_targets.erase(normalized_target);
    }

    std::filesystem::path cache_path = cjsh_filesystem::g_cjsh_generated_completions_path() /
                                       (sanitize_command_for_cache(target) + ".txt");

    if (force_refresh) {
        std::error_code remove_error;
        (void)std::filesystem::remove(cache_path, remove_error);
    }

    CommandDoc doc = load_entries_for_target(target, true, true);
    result.generated = !doc.entries.empty() || !doc.summary.empty();

    if (!discover_subcommands) {
        return result;
    }

    for (const auto& entry : doc.entries) {
        if (entry.kind != EntryKind::Subcommand) {
            continue;
        }

        std::string subcommand = normalize_subcommand_token(entry.text);
        if (!is_token_allowed_for_combination(subcommand)) {
            continue;
        }

        result.discovered_targets.push_back(std::string(target).append("-").append(subcommand));
    }

    return result;
}

void handle_external_sub_completions(
    ic_completion_env_t* cenv, const completion_context::CommandLineContext& command_context) {
    if (cenv == nullptr) {
        return;
    }
    if (ic_stop_completing(cenv)) {
        return;
    }

    const std::vector<std::string>& tokens = command_context.effective_tokens;
    if (tokens.empty()) {
        return;
    }

    bool ends_with_space = command_context.at_word_boundary;

    std::size_t stable_count = tokens.size();
    if (!ends_with_space && !tokens.empty()) {
        stable_count = tokens.size() - 1;
    }
    if (stable_count == 0) {
        stable_count = 1;
    }

    std::string current_prefix;
    if (!ends_with_space && !tokens.empty()) {
        current_prefix = command_context.current_prefix;
    }

    const bool is_hint = ic_completion_is_hint(cenv);
    const bool allow_fetch = !is_hint && config::completion_learning_enabled &&
                             !cjsh_filesystem::find_executable_in_path(tokens.front()).empty();

    auto context = resolve_completion_context(tokens, stable_count, allow_fetch, !is_hint);
    long delete_before = command_context.current_raw_prefix.empty()
                             ? 0
                             : static_cast<long>(command_context.current_raw_prefix.size());
    std::size_t added = 0;

    auto add_value_completions = [&](const CompletionEntry& entry, const std::string& value_prefix,
                                     const std::string& replacement_base) {
        std::unordered_set<std::string> seen_values;
        auto values = collect_value_candidates(entry, tokens, context, value_prefix, !is_hint);
        std::stable_partition(values.begin(), values.end(), [&](const auto& candidate) {
            return completion_tracker::is_completion_preferred(
                (replacement_base + candidate.value).c_str(), delete_before);
        });
        for (const auto& candidate : values) {
            if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv) ||
                added >= 120) {
                break;
            }
            if (candidate.value.empty() || !seen_values.insert(candidate.value).second ||
                !completion_utils::matches_completion_prefix(candidate.value, value_prefix)) {
                continue;
            }

            std::string insert_text = replacement_base + candidate.value;
            if (!insert_text.empty() && insert_text.back() != ' ') {
                insert_text.push_back(' ');
            }
            std::string source =
                candidate.description.empty()
                    ? (!entry.description.empty() ? entry.description : entry.value.name)
                    : candidate.description;
            if (entry.deprecated) {
                source.insert(0, "deprecated · ");
            }
            if (!completion_tracker::safe_add_completion_prim_with_source(
                    cenv, insert_text.c_str(), nullptr, nullptr, source.c_str(), delete_before,
                    0)) {
                break;
            }
            ++added;
        }
    };

    std::optional<CompletionEntry> value_entry = context.pending_value;
    std::string value_prefix = current_prefix;
    std::string replacement_base;

    if (!value_entry.has_value() && context.options_enabled && !current_prefix.empty() &&
        current_prefix.front() == '-') {
        std::size_t equals = current_prefix.find('=');
        if (equals != std::string::npos) {
            std::string option_name = current_prefix.substr(0, equals);
            const CompletionEntry* option =
                find_entry(context.entries, EntryKind::Option, option_name);
            if (option != nullptr && option->value.requirement != ValueRequirement::None) {
                value_entry = *option;
                replacement_base = current_prefix.substr(0, equals + 1);
                value_prefix = current_prefix.substr(equals + 1);
            }
        }
    }

    if (value_entry.has_value()) {
        add_value_completions(*value_entry, value_prefix, replacement_base);
        return;
    }

    const CompletionEntry* positional =
        positional_entry_for_index(context.entries, context.positional_count + 1);
    if (positional != nullptr &&
        (!context.options_enabled || current_prefix.empty() || current_prefix.front() != '-')) {
        add_value_completions(*positional, current_prefix, {});
    }

    struct Candidate {
        std::string insert_text;
        const CompletionEntry* entry;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : context.entries) {
        if (entry.kind == EntryKind::Positional || !entry_is_available(entry, context)) {
            continue;
        }
        if (!context.options_enabled &&
            (entry.kind == EntryKind::Option || entry.kind == EntryKind::Subcommand)) {
            continue;
        }

        for (std::string candidate_name : completion_specs::entry_names(entry)) {
            if (entry.kind == EntryKind::Subcommand) {
                candidate_name = normalize_subcommand_token(candidate_name);
            }

            if (!current_prefix.empty() &&
                !completion_utils::matches_completion_prefix(candidate_name, current_prefix)) {
                continue;
            }

            std::string insert_text = candidate_name;
            bool append_space = entry.kind == EntryKind::Subcommand;
            if (entry.kind == EntryKind::Option) {
                if (entry.value.requirement != ValueRequirement::None &&
                    entry.value.separator == ValueSeparator::Equals) {
                    insert_text.push_back('=');
                } else {
                    append_space = true;
                }
            }

            if (append_space && !insert_text.empty() && insert_text.back() != ' ') {
                insert_text.push_back(' ');
            }

            candidates.push_back({std::move(insert_text), &entry});
        }
    }
    std::stable_partition(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
        return completion_tracker::is_completion_preferred(candidate.insert_text.c_str(),
                                                           delete_before);
    });
    for (const auto& candidate : candidates) {
        if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv) ||
            added >= 120) {
            break;
        }
        const auto& entry = *candidate.entry;
        std::string source = entry.description.empty()
                                 ? (entry.kind == EntryKind::Subcommand ? "subcommand" : "option")
                                 : entry.description;
        if (entry.deprecated) {
            source.insert(0, "deprecated · ");
        }

        if (!completion_tracker::safe_add_completion_prim_with_source(
                cenv, candidate.insert_text.c_str(), nullptr, nullptr, source.c_str(),
                delete_before, 0)) {
            return;
        }
        ++added;
    }
}
