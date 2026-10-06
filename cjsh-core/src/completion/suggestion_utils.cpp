/*
  suggestion_utils.cpp

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

#include "suggestion_utils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "builtin.h"
#include "cjsh_filesystem.h"
#include "completion_spell.h"
#include "completion_utils.h"
#include "edit_distance_utils.h"
#include "interpreter.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"

namespace suggestion_utils {

CdLookupContext build_cd_lookup_context(const std::string& target_dir,
                                        const std::string& current_dir) {
    CdLookupContext context;
    context.current_path = std::filesystem::path(current_dir);
    context.target_path = std::filesystem::path(target_dir);
    context.base_path = context.current_path;
    context.lookup_fragment = target_dir;

    std::error_code ec;

    if (!target_dir.empty()) {
        if (context.target_path.is_absolute()) {
            context.base_path = context.target_path.parent_path();
            context.lookup_fragment = context.target_path.filename().string();
            if (context.lookup_fragment.empty()) {
                context.lookup_fragment = context.target_path.string();
            }
        } else {
            std::filesystem::path resolved = context.current_path / context.target_path;
            std::filesystem::path parent = resolved.parent_path();
            if (parent.empty()) {
                parent = context.current_path;
            }

            if (std::filesystem::exists(parent, ec)) {
                context.base_path = std::move(parent);
                context.lookup_fragment = resolved.filename().string();
                if (context.lookup_fragment.empty()) {
                    context.lookup_fragment = context.target_path.filename().string();
                    if (context.lookup_fragment.empty()) {
                        context.lookup_fragment = target_dir;
                    }
                }
            } else {
                ec.clear();
            }
        }
    }

    if (context.lookup_fragment.empty()) {
        context.lookup_fragment = target_dir;
    }

    context.base_dir = context.base_path.empty() ? current_dir : context.base_path.string();
    context.search_base =
        context.base_path.empty() ? std::filesystem::path(context.base_dir) : context.base_path;
    return context;
}

std::vector<std::string> generate_command_suggestions(const std::string& command) {
    std::vector<std::string> suggestions;

    if (command.empty()) {
        return suggestions;
    }

    std::string normalized_command = completion_utils::normalize_for_comparison(command);
    if (!completion_spell::should_consider_spell_correction(normalized_command)) {
        return suggestions;
    }

    const cjsh_filesystem::ScopedInteractivePathLookup path_lookup;
    std::unordered_set<std::string> shell_commands;

    if (g_shell && g_shell->get_built_ins()) {
        auto builtin_commands = g_shell->get_built_ins()->get_builtin_commands();
        for (const auto& builtin : builtin_commands) {
            (void)shell_commands.insert(builtin);
        }
    }

    if (g_shell) {
        auto& aliases = g_shell->get_aliases();
        for (const auto& alias_pair : aliases) {
            (void)shell_commands.insert(alias_pair.first);
        }
    }

    if (g_shell) {
        auto& abbreviations = g_shell->get_abbreviations();
        for (const auto& abbr_pair : abbreviations) {
            (void)shell_commands.insert(abbr_pair.first);
        }
    }

    if (g_shell && g_shell->get_shell_script_interpreter()) {
        auto function_names = g_shell->get_shell_script_interpreter()->get_function_names();
        for (const auto& func_name : function_names) {
            (void)shell_commands.insert(func_name);
        }
    }

    auto all_commands = cjsh_filesystem::get_path_completion_candidates();
    all_commands.insert(all_commands.end(), shell_commands.begin(), shell_commands.end());

    if (all_commands.empty()) {
        return suggestions;
    }

    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> spell_matches;
    completion_spell::collect_spell_correction_candidates(
        all_commands, [](const std::string& value) { return value; },
        [&](const std::string& candidate) {
            // Spell matching runs before this filter, so unrelated PATH files need no metadata
            // lookup. Shell commands remain valid even if a nonexecutable file shares the name.
            return shell_commands.count(candidate) != 0 ||
                   !cjsh_filesystem::find_executable_in_path(candidate).empty();
        },
        normalized_command, spell_matches);

    if (spell_matches.empty()) {
        return suggestions;
    }

    auto ordered_matches = completion_spell::order_spell_correction_matches(spell_matches);
    for (size_t i = 0; i < ordered_matches.size() && i < 5; ++i) {
        suggestions.push_back("Did you mean '" + ordered_matches[i].candidate + "'?");
    }

    return suggestions;
}

std::vector<std::string> generate_command_suggestions_if_enabled(const std::string& command) {
    if (!config::error_suggestions_enabled || command.empty()) {
        return {};
    }
    return generate_command_suggestions(command);
}

std::vector<std::string> generate_cd_suggestions(const std::string& target_dir,
                                                 const std::string& current_dir) {
    std::vector<std::string> suggestions;

    CdLookupContext context = build_cd_lookup_context(target_dir, current_dir);

    std::error_code ec;

    auto raw_similar = find_similar_entries(context.lookup_fragment, context.base_dir, 5);

    std::vector<std::string> similar;
    std::unordered_set<std::string> seen_paths;

    for (const auto& candidate : raw_similar) {
        std::filesystem::path candidate_path = context.search_base / candidate;

        if (!std::filesystem::exists(candidate_path, ec)) {
            ec.clear();
            continue;
        }

        if (!std::filesystem::is_directory(candidate_path, ec)) {
            ec.clear();
            continue;
        }

        std::string display_value;
        if (context.target_path.is_absolute()) {
            display_value = candidate_path.string();
        } else {
            auto relative_candidate =
                std::filesystem::relative(candidate_path, context.current_path, ec);
            if (!ec && !relative_candidate.empty() && relative_candidate.string() != ".") {
                display_value = relative_candidate.string();
            } else {
                ec.clear();
                display_value = candidate_path.string();
            }
        }

        if (display_value.empty()) {
            display_value = candidate;
        }

        if (seen_paths.insert(display_value).second) {
            similar.push_back(display_value);
        }
    }

    suggestions.reserve(similar.size());
    for (const auto& dir : similar) {
        suggestions.push_back("Did you mean 'cd " + dir + "'?");
    }

    if (target_dir.find('/') == std::string::npos) {
        if (similar.empty()) {
            suggestions.push_back("Try 'ls' to see available directories.");
        }
        if (target_dir != "..") {
            suggestions.push_back("Use 'cd ..' to go to parent directory.");
        }
    } else {
        std::string parent_path = target_dir.substr(0, target_dir.find_last_of('/'));
        if (!parent_path.empty() && parent_path != target_dir) {
            suggestions.push_back("Check if '" + parent_path + "' exists first.");
        }
    }

    return suggestions;
}

int edit_distance(const std::string& str1, const std::string& str2) {
    return edit_distance_utils::levenshtein_distance(str1, str2);
}

std::vector<std::string> find_similar_entries(const std::string& target_name,
                                              const std::string& directory, int max_suggestions) {
    std::vector<std::string> suggestions;

    if (target_name.empty()) {
        return suggestions;
    }

    std::string target_lower = string_utils::to_lower_copy(target_name);

    try {
        std::vector<std::pair<int, std::string>> candidates;

        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            std::string name = entry.path().filename().string();

            if (name.empty()) {
                continue;
            }

            if (name[0] == '.' && target_name[0] != '.') {
                continue;
            }

            std::string name_lower = string_utils::to_lower_copy(name);

            bool substring_match = name_lower.find(target_lower) != std::string::npos;

            int distance = edit_distance(target_name, name);
            int max_distance =
                std::max(3, static_cast<int>(std::max(target_name.length(), name.length()) / 2));

            if (!substring_match && (distance == 0 || distance > max_distance)) {
                continue;
            }

            int score = 1000 - distance * 10;

            if (substring_match) {
                score += 200;
                if (name_lower.rfind(target_lower, 0) == 0) {
                    score += 150;
                }
                score -= static_cast<int>(
                    std::max<size_t>(0, name_lower.length() - target_lower.length()));
            }

            if (!target_lower.empty() && !name_lower.empty() && target_lower[0] == name_lower[0]) {
                score += 50;
            }

            int consecutive_matches = 0;
            size_t name_idx = 0;
            for (size_t i = 0; i < target_lower.length() && name_idx < name_lower.length(); i++) {
                bool found = false;
                for (size_t j = name_idx; j < name_lower.length(); j++) {
                    if (target_lower[i] == name_lower[j]) {
                        consecutive_matches++;
                        name_idx = j + 1;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    break;
                }
            }

            if (consecutive_matches >= static_cast<int>(target_name.length() * 0.8)) {
                score += 200;
            }

            (void)candidates.emplace_back(score, name);
        }

        std::sort(candidates.begin(), candidates.end(),
                  std::greater<std::pair<int, std::string>>());

        if (candidates.empty()) {
            return suggestions;
        }

        constexpr double kSimilarityRetentionRatio = 0.65;
        constexpr int kSimilarityGapAllowance = 250;

        int best_score = candidates.front().first;
        int min_score = std::max(best_score - kSimilarityGapAllowance,
                                 static_cast<int>(best_score * kSimilarityRetentionRatio));

        for (size_t i = 0;
             i < candidates.size() && suggestions.size() < static_cast<size_t>(max_suggestions);
             i++) {
            if (i > 0 && candidates[i].first < min_score) {
                break;
            }
            suggestions.push_back(candidates[i].second);
        }

    } catch (const std::filesystem::filesystem_error&) {
        // Ignore filesystem errors; return suggestions collected so far.
    }

    return suggestions;
}

}  // namespace suggestion_utils
