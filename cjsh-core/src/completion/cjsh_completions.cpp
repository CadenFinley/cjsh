/*
  cjsh_completions.cpp

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

#include "cjsh_completions.h"
#include <iterator>
#include "isocline.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "builtin.h"
#include "builtins_completions_handler.h"
#include "cjsh_filesystem.h"
#include "command_lookup.h"
#include "completion_context.h"
#include "completion_history.h"
#include "completion_spell.h"
#include "completion_tracker.h"
#include "completion_utils.h"
#include "error_out.h"
#include "external_sub_completions.h"
#include "interpreter.h"
#include "isocline/isocline.h"
#include "job_control.h"
#include "parser_utils.h"
#include "quote_state.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"
#include "token_constants.h"

namespace {
bool g_completion_case_sensitive = false;
bool g_completion_spell_correction_enabled = true;
bool g_completion_spell_correction_on_enter_enabled = false;
}  // namespace

enum CompletionContext : std::uint8_t {
    CONTEXT_COMMAND,
    CONTEXT_ARGUMENT,
    CONTEXT_PATH
};

namespace {

const char* extract_current_line_prefix(const char* prefix) {
    if (prefix == nullptr) {
        return "";
    }

    const char* last_newline = strrchr(prefix, '\n');
    const char* last_carriage = strrchr(prefix, '\r');
    const char* last_break = last_newline;

    if (last_carriage != nullptr && (last_break == nullptr || last_carriage > last_break)) {
        last_break = last_carriage;
    }

    if (last_break != nullptr) {
        return last_break + 1;
    }

    return prefix;
}

struct CommandSubstitutionFrame {
    size_t content_start;
    int paren_depth;
};

size_t find_innermost_unclosed_command_substitution_start(const std::string& prefix) {
    std::vector<CommandSubstitutionFrame> stack;
    stack.reserve(4);

    utils::QuoteState quote_state;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char ch = prefix[i];
        const bool was_escaped = quote_state.escaped;
        const bool was_in_single_quote = quote_state.in_single_quote;
        const auto advance_result = quote_state.consume_forward(ch);

        if (advance_result == utils::QuoteAdvanceResult::Continue) {
            continue;
        }

        if (!was_escaped && !was_in_single_quote && ch == '$' && i + 1 < prefix.size() &&
            prefix[i + 1] == '(' && (i + 2 >= prefix.size() || prefix[i + 2] != '(')) {
            stack.push_back(CommandSubstitutionFrame{i + 2, 1});
            ++i;
            continue;
        }

        if (stack.empty() || quote_state.inside_quotes()) {
            continue;
        }

        if (ch == '(') {
            ++stack.back().paren_depth;
            continue;
        }

        if (ch == ')') {
            --stack.back().paren_depth;
            if (stack.back().paren_depth <= 0) {
                stack.pop_back();
            }
        }
    }

    if (stack.empty()) {
        return std::string::npos;
    }

    return stack.back().content_start;
}

std::string extract_completion_scope_prefix(const char* prefix) {
    const char* current_line_prefix = extract_current_line_prefix(prefix);
    std::string scoped_prefix = current_line_prefix ? current_line_prefix : "";

    size_t command_substitution_start =
        find_innermost_unclosed_command_substitution_start(scoped_prefix);
    if (command_substitution_start != std::string::npos &&
        command_substitution_start <= scoped_prefix.size()) {
        return scoped_prefix.substr(command_substitution_start);
    }

    return scoped_prefix;
}

bool prepare_prefix_state(ic_completion_env_t* cenv, const char* prefix, std::string& prefix_str,
                          size_t& prefix_len) {
    if (ic_stop_completing(cenv)) {
        return false;
    }
    if (completion_tracker::completion_limit_hit()) {
        return false;
    }

    prefix_str = prefix ? prefix : "";
    prefix_len = prefix_str.length();
    return true;
}

bool decode_history_command_line(const std::string& raw, std::string& decoded) {
    decoded.resize(raw.size() + 1);
    size_t decoded_length = 0;
    if (!ic_history_decode_entry(raw.data(), raw.size(), decoded.data(), decoded.size(),
                                 &decoded_length)) {
        decoded.clear();
        return false;
    }
    decoded.resize(decoded_length);
    return true;
}

bool add_command_completion(ic_completion_env_t* cenv, const std::string& candidate,
                            size_t prefix_len, const char* source) {
    long delete_before = static_cast<long>(prefix_len);
    std::string completion_text = candidate;
    if (completion_text.empty() || completion_text.back() != ' ') {
        completion_text.push_back(' ');
    }
    return completion_tracker::safe_add_completion_prim_with_source(
        cenv, completion_text.c_str(), nullptr, nullptr, source, delete_before, 0);
}

constexpr int kHistoryCompletionHiddenExitCode = 127;

bool add_path_completion(ic_completion_env_t* cenv, const char* source, long delete_before,
                         const std::string& completion_suffix) {
    if (delete_before == 0) {
        return completion_tracker::safe_add_completion_with_source(cenv, completion_suffix.c_str(),
                                                                   source);
    }
    return completion_tracker::safe_add_completion_prim_with_source(
        cenv, completion_suffix.c_str(), nullptr, nullptr, source, delete_before, 0);
}

void determine_directory_target(const std::string& path, bool treat_as_directory,
                                std::filesystem::path& dir_path, std::string& match_prefix) {
    namespace fs = std::filesystem;
    if (treat_as_directory || path.empty() || path.back() == '/') {
        dir_path = path.empty() ? fs::path(".") : fs::path(path);
        match_prefix.clear();
        return;
    }
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string directory_part = path.substr(0, last_slash);
        if (directory_part.empty()) {
            directory_part = "/";
        }
        dir_path = directory_part;
        match_prefix = path.substr(last_slash + 1);
    } else {
        dir_path = ".";
        match_prefix = path;
    }
}

bool has_shebang_line(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    char prefix[2];
    (void)file.read(prefix, sizeof(prefix));
    return file.gcount() == static_cast<std::streamsize>(sizeof(prefix)) && prefix[0] == '#' &&
           prefix[1] == '!';
}

struct CompletionEntry {
    std::string filename;
    std::string sort_key;
    bool directory = false;
    bool runnable = false;
    const char* source = "file";

    int priority() const {
        return runnable ? 0 : directory ? 1 : 2;
    }
};

enum class CompletionInspection : std::uint8_t {
    TypeOnly,
    Runnable,
    Source,
    RunnableAndSource
};

CompletionEntry inspect_completion_entry(const std::filesystem::directory_entry& entry,
                                         CompletionInspection inspection) {
    namespace fs = std::filesystem;
    CompletionEntry result;
    std::error_code ec;
    const auto status = entry.status(ec);
    if (ec) {
        return result;
    }
    result.directory = fs::is_directory(status);
    if (result.directory) {
        result.source = "directory";
    } else if (inspection != CompletionInspection::TypeOnly && fs::is_regular_file(status)) {
        constexpr auto exec_mask =
            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
        const bool executable = (status.permissions() & exec_mask) != fs::perms::none;
        const bool check_runnable = inspection != CompletionInspection::Source;
        const bool classify_source = inspection != CompletionInspection::Runnable;
        const bool script = ((executable && classify_source) || (!executable && check_runnable)) &&
                            has_shebang_line(entry.path());
        result.runnable = executable || script;
        if (executable) {
            result.source = script ? "executable script" : "executable binary";
        }
    }
    return result;
}

bool is_executable_or_script_entry(const std::filesystem::directory_entry& entry) {
    const auto info = inspect_completion_entry(entry, CompletionInspection::Runnable);
    return info.directory || info.runnable;
}

template <typename Container, typename Extractor>
void process_command_candidates(
    ic_completion_env_t* cenv, const Container& container, const std::string& prefix,
    size_t prefix_len, const char* source, Extractor extractor,
    const std::function<bool(const std::string&)>& filter = {},
    const std::function<std::string(const std::string&)>& source_provider = {}) {
    for (const auto& item : container) {
        if (completion_tracker::completion_limit_hit()) {
            return;
        }
        if (ic_stop_completing(cenv)) {
            return;
        }
        std::string candidate = extractor(item);
        if (!completion_utils::matches_completion_prefix(candidate, prefix)) {
            continue;
        }
        if (filter && !filter(candidate)) {
            continue;
        }
        const char* source_ptr = source;
        std::string dynamic_source;
        if (source_provider) {
            dynamic_source = source_provider(candidate);
            if (!dynamic_source.empty()) {
                source_ptr = dynamic_source.c_str();
            }
        }
        if (!add_command_completion(cenv, candidate, prefix_len, source_ptr)) {
            return;
        }
        if (ic_stop_completing(cenv)) {
            return;
        }
    }
}

bool iterate_directory_entries(ic_completion_env_t* cenv, const std::filesystem::path& dir_path,
                               const std::string& match_prefix, bool directories_only,
                               bool skip_hidden_without_prefix,
                               bool restrict_to_executables = false,
                               bool prioritize_runnable_entries = false) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir_path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return true;
    }

    const long delete_before = static_cast<long>(match_prefix.length());
    auto emit_completion = [&](const CompletionEntry& entry) {
        if (ic_stop_completing(cenv) || completion_tracker::completion_limit_hit()) {
            return false;
        }
        std::string suffix = completion_utils::quote_path_if_needed(entry.filename);
        suffix += entry.directory ? "/" : " ";
        return add_path_completion(cenv, entry.source, delete_before, suffix) &&
               !ic_stop_completing(cenv);
    };

    std::vector<CompletionEntry> deferred_entries;
    if (prioritize_runnable_entries) {
        deferred_entries.reserve(32);
    }
    const auto inspection = directories_only ? CompletionInspection::TypeOnly
                            : restrict_to_executables || prioritize_runnable_entries
                                ? CompletionInspection::RunnableAndSource
                                : CompletionInspection::Source;
    for (; it != fs::directory_iterator(); (void)it.increment(ec)) {
        if (ec) {
            break;
        }
        if (ic_stop_completing(cenv) || completion_tracker::completion_limit_hit()) {
            return false;
        }
        std::string filename = it->path().filename().string();
        if (filename.empty() ||
            (skip_hidden_without_prefix && match_prefix.empty() && filename[0] == '.') ||
            (!match_prefix.empty() &&
             !completion_utils::matches_completion_prefix(filename, match_prefix))) {
            continue;
        }

        auto entry = inspect_completion_entry(*it, inspection);
        if ((directories_only && !entry.directory) ||
            (restrict_to_executables && !entry.directory && !entry.runnable)) {
            continue;
        }
        entry.filename = std::move(filename);
        if (prioritize_runnable_entries) {
            entry.sort_key = g_completion_case_sensitive
                                 ? entry.filename
                                 : completion_utils::normalize_for_comparison(entry.filename);
            deferred_entries.push_back(std::move(entry));
        } else if (!emit_completion(entry)) {
            return false;
        }
    }

    std::sort(deferred_entries.begin(), deferred_entries.end(),
              [](const CompletionEntry& lhs, const CompletionEntry& rhs) {
                  if (lhs.priority() != rhs.priority()) {
                      return lhs.priority() < rhs.priority();
                  }
                  return lhs.sort_key == rhs.sort_key ? lhs.filename < rhs.filename
                                                      : lhs.sort_key < rhs.sort_key;
              });
    return std::all_of(deferred_entries.begin(), deferred_entries.end(), emit_completion);
}

bool is_interactive_builtin(const std::string& cmd) {
    static const std::unordered_set<std::string> script_only_builtins = {"__INTERNAL_SUBSHELL__"};

    return script_only_builtins.find(cmd) == script_only_builtins.end();
}

std::vector<std::string> collect_map_keys(
    const std::unordered_map<std::string, std::string>& values) {
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& entry : values) {
        keys.push_back(entry.first);
    }
    return keys;
}

std::function<std::string(const std::string&)> make_map_source_provider(
    const std::unordered_map<std::string, std::string>* values) {
    return [values](const std::string& name) -> std::string {
        if (values == nullptr) {
            return {};
        }
        auto it = values->find(name);
        if (it == values->end()) {
            return {};
        }
        return it->second;
    };
}

std::string builtin_summary_for_command(const std::string& cmd) {
    return builtin_completions::get_builtin_summary(cmd);
}

void add_builtin_command_candidates(ic_completion_env_t* cenv,
                                    const std::vector<std::string>& builtin_cmds,
                                    const std::string& prefix, size_t prefix_len) {
    auto builtin_filter = [](const std::string& cmd) { return is_interactive_builtin(cmd); };
    process_command_candidates(
        cenv, builtin_cmds, prefix, prefix_len, "builtin",
        [](const std::string& value) { return value; }, builtin_filter,
        builtin_summary_for_command);
}

struct CommandCompletionSources {
    std::vector<std::string> builtin_cmds;
    std::vector<std::string> function_names;
    std::vector<std::string> alias_names;
    std::vector<std::string> abbreviation_names;
    std::vector<std::string> executables_in_path;
    const std::unordered_map<std::string, std::string>* alias_map = nullptr;
    const std::unordered_map<std::string, std::string>* abbreviation_map = nullptr;
};

CommandCompletionSources collect_command_completion_sources() {
    CommandCompletionSources sources;
    if (g_shell && (g_shell->get_built_ins() != nullptr)) {
        sources.builtin_cmds = g_shell->get_built_ins()->get_builtin_commands();
    }

    if (g_shell && (g_shell->get_shell_script_interpreter() != nullptr)) {
        sources.function_names = g_shell->get_shell_script_interpreter()->get_function_names();
    }

    if (g_shell) {
        sources.alias_map = &g_shell->get_aliases();
        sources.alias_names = collect_map_keys(*sources.alias_map);

        sources.abbreviation_map = &g_shell->get_abbreviations();
        sources.abbreviation_names = collect_map_keys(*sources.abbreviation_map);
    }

    sources.executables_in_path = cjsh_filesystem::get_path_completion_candidates();

    return sources;
}

bool command_resolution_is_unknown(const std::string& token) {
    if (token.empty()) {
        return false;
    }

    // Assignments occupy command position syntactically, but are not commands. In particular,
    // multiline input commonly starts a continuation line with an assignment such as `i=2`.
    // Do not compare those tokens with command names for spell correction.
    if (looks_like_assignment(token)) {
        return false;
    }

    // This is an existence query, not a request for every resolution (as in
    // `type -a`). A known shell command needs no filesystem work, particularly
    // no interactive PATH index rebuild on each argument-completion request.
    const auto resolution = command_lookup::resolve_command(token, g_shell.get(), false);
    if (resolution.is_keyword || resolution.is_builtin || resolution.has_alias ||
        resolution.has_function) {
        return false;
    }

    if (command_lookup::should_auto_cd_token(token, g_shell.get())) {
        return false;
    }

    return cjsh_filesystem::find_executable_in_path(token).empty();
}

bool cursor_is_inside_known_command(const char* input, long cursor,
                                    const completion_context::CommandLineContext& context) {
    if (input == nullptr || cursor < 0 || !context.cursor_in_command_position ||
        context.current_raw_prefix.empty()) {
        return false;
    }

    const std::string_view full_input(input);
    const auto position = static_cast<std::size_t>(cursor);
    if (position >= full_input.size() || context.current_raw_prefix.size() > position) {
        return false;
    }

    // Completion uses a prefix scoped to the current command. Inspect the rest
    // of that word before treating a prefix such as t|hen as unfinished input.
    const std::size_t start = position - context.current_raw_prefix.size();
    std::size_t end = start;
    utils::QuoteState quote_state;
    for (; end < full_input.size(); ++end) {
        const char ch = full_input[end];
        if (quote_state.consume_forward(ch) == utils::QuoteAdvanceResult::Continue) {
            continue;
        }
        if (!quote_state.inside_quotes() && (std::isspace(static_cast<unsigned char>(ch)) != 0 ||
                                             std::strchr("|&;()<>", ch) != nullptr)) {
            break;
        }
    }

    if (end <= position || quote_state.inside_quotes() || quote_state.escaped) {
        return false;
    }
    const std::string word =
        completion_utils::unquote_path(std::string(full_input.substr(start, end - start)));
    return !command_resolution_is_unknown(word);
}

void add_command_spell_corrections(ic_completion_env_t* cenv,
                                   const CommandCompletionSources& sources,
                                   const std::string& normalized_prefix,
                                   size_t delete_before_length, bool high_confidence_only = false) {
    if ((!high_confidence_only && ic_has_completions(cenv)) ||
        !g_completion_spell_correction_enabled) {
        return;
    }

    if (!completion_spell::should_consider_spell_correction(normalized_prefix)) {
        return;
    }

    std::unordered_map<std::string, completion_spell::SpellCorrectionMatch> spell_matches;

    completion_spell::collect_spell_correction_candidates(
        sources.builtin_cmds, [](const std::string& value) { return value; },
        [](const std::string& cmd) { return is_interactive_builtin(cmd); }, normalized_prefix,
        spell_matches, high_confidence_only);

    completion_spell::collect_spell_correction_candidates(
        sources.function_names, [](const std::string& value) { return value; },
        std::function<bool(const std::string&)>{}, normalized_prefix, spell_matches,
        high_confidence_only);

    completion_spell::collect_spell_correction_candidates(
        sources.alias_names, [](const std::string& value) { return value; },
        std::function<bool(const std::string&)>{}, normalized_prefix, spell_matches,
        high_confidence_only);

    completion_spell::collect_spell_correction_candidates(
        sources.abbreviation_names, [](const std::string& value) { return value; },
        std::function<bool(const std::string&)>{}, normalized_prefix, spell_matches,
        high_confidence_only);

    completion_spell::collect_spell_correction_candidates(
        sources.executables_in_path, [](const std::string& value) { return value; },
        [&](const std::string& candidate) {
            return !cjsh_filesystem::find_executable_in_path(candidate).empty();
        },
        normalized_prefix, spell_matches, high_confidence_only);

    if (!spell_matches.empty()) {
        completion_spell::add_spell_correction_matches(cenv, spell_matches, delete_before_length);
    }
}

void add_command_name_completions(ic_completion_env_t* cenv,
                                  const CommandCompletionSources& sources,
                                  const std::string& prefix, size_t delete_before_length,
                                  bool allow_spell_corrections = true) {
    std::string normalized_prefix;
    const bool should_offer_spell_corrections =
        allow_spell_corrections && command_resolution_is_unknown(prefix);
    if (should_offer_spell_corrections) {
        normalized_prefix = completion_utils::normalize_for_comparison(prefix);
        add_command_spell_corrections(cenv, sources, normalized_prefix, delete_before_length, true);
    }

    if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv)) {
        return;
    }

    size_t summary_fetch_budget = delete_before_length == 0 ? 2 : 5;
    auto system_summary_provider = [&](const std::string& cmd) -> std::string {
        std::string summary = get_command_summary(cmd, false);
        if (!summary.empty()) {
            return summary;
        }
        if (ic_completion_is_hint(cenv) || !config::completion_learning_enabled ||
            summary_fetch_budget == 0) {
            return {};
        }
        --summary_fetch_budget;
        return get_command_summary(cmd, true);
    };

    struct CandidateGroup {
        const std::vector<std::string>& names;
        const char* source;
        std::function<bool(const std::string&)> filter;
        std::function<std::string(const std::string&)> describe;
    };
    const CandidateGroup groups[] = {
        {sources.builtin_cmds, "builtin", is_interactive_builtin, builtin_summary_for_command},
        {command_lookup::shell_control_structure_keywords(),
         "control structure",
         {},
         builtin_summary_for_command},
        {sources.function_names, "function", {}, {}},
        {sources.alias_names, "alias", {}, make_map_source_provider(sources.alias_map)},
        {sources.abbreviation_names,
         "abbreviation",
         {},
         make_map_source_provider(sources.abbreviation_map)},
        {sources.executables_in_path, "system installed command",
         [](const std::string& candidate) {
             return !cjsh_filesystem::find_executable_in_path(candidate).empty();
         },
         system_summary_provider},
    };
    struct Candidate {
        const std::string* name;
        const CandidateGroup* group;
        std::string sort_key;
    };
    std::vector<Candidate> candidates;
    for (const auto& group : groups) {
        for (const auto& name : group.names) {
            if (completion_utils::matches_completion_prefix(name, prefix)) {
                candidates.push_back(
                    {&name, &group, completion_utils::normalize_for_comparison(name)});
            }
        }
    }
    // Rank all matching command names before spending the result budget. A
    // two-result hint must see the same leading candidates as a full menu.
    // Stability preserves source precedence when the same name occurs twice.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& lhs, const Candidate& rhs) {
                         if (lhs.name->size() != rhs.name->size()) {
                             return lhs.name->size() < rhs.name->size();
                         }
                         return lhs.sort_key == rhs.sort_key ? *lhs.name < *rhs.name
                                                             : lhs.sort_key < rhs.sort_key;
                     });
    for (const auto& candidate : candidates) {
        if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv)) {
            break;
        }
        const auto& group = *candidate.group;
        const auto& name = *candidate.name;
        // Validate only candidates we can emit; ranking must not stat every
        // PATH entry or fetch documentation for entries beyond the limit.
        if (group.filter && !group.filter(name)) {
            continue;
        }
        const std::string description = group.describe ? group.describe(name) : std::string{};
        const char* source = description.empty() ? group.source : description.c_str();
        if (!add_command_completion(cenv, name, delete_before_length, source)) {
            break;
        }
    }

    if (should_offer_spell_corrections) {
        add_command_spell_corrections(cenv, sources, normalized_prefix, delete_before_length);
    }
}

bool add_split_unknown_command_completions(ic_completion_env_t* cenv,
                                           const std::vector<std::string>& tokens,
                                           bool ends_with_space, const std::string& full_prefix) {
    if (cenv == nullptr || ends_with_space || tokens.size() != 2 || ic_stop_completing(cenv) ||
        completion_tracker::completion_limit_hit()) {
        return false;
    }

    const std::string& first_token = tokens[0];
    const std::string& second_token = tokens[1];
    if (!command_lookup::token_allows_split_command_merge(first_token) ||
        !command_lookup::token_allows_split_command_merge(second_token)) {
        return false;
    }

    if (!command_resolution_is_unknown(first_token) ||
        !command_resolution_is_unknown(second_token)) {
        return false;
    }

    std::string merged_prefix = first_token + second_token;
    if (merged_prefix.empty()) {
        return false;
    }

    auto sources = collect_command_completion_sources();
    add_command_name_completions(cenv, sources, merged_prefix, full_prefix.length(), false);
    return ic_has_completions(cenv);
}

bool is_valid_variable_completion_prefix(const std::string& prefix) {
    return std::all_of(prefix.begin(), prefix.end(), [](char ch) {
        unsigned char uch = static_cast<unsigned char>(ch);
        return std::isalnum(uch) != 0 || ch == '_' || ch == '?' || ch == '$' || ch == '#' ||
               ch == '*' || ch == '@' || ch == '!';
    });
}

bool find_last_expandable_dollar(const std::string& token, bool& braced, size_t& var_start) {
    utils::QuoteState quote_state;
    size_t last_dollar = std::string::npos;
    bool last_braced = false;
    size_t last_var_start = std::string::npos;

    for (size_t i = 0; i < token.size(); ++i) {
        char c = token[i];
        if (quote_state.consume_forward(c) == utils::QuoteAdvanceResult::Continue) {
            continue;
        }

        if (c == '$' && !quote_state.in_single_quote) {
            last_dollar = i;
            last_braced = (i + 1 < token.size() && token[i + 1] == '{');
            last_var_start = last_braced ? i + 2 : i + 1;
        }
    }

    if (last_dollar == std::string::npos) {
        return false;
    }

    braced = last_braced;
    var_start = last_var_start;
    return true;
}

bool add_variable_completions(ic_completion_env_t* cenv, const std::string& prefix) {
    if (ic_stop_completing(cenv) || completion_tracker::completion_limit_hit()) {
        return false;
    }

    size_t last_space = completion_utils::find_last_unquoted_space(prefix);
    std::string token_prefix =
        (last_space == std::string::npos) ? prefix : prefix.substr(last_space + 1);

    if (token_prefix.empty()) {
        return false;
    }

    bool braced = false;
    size_t var_start = std::string::npos;
    if (!find_last_expandable_dollar(token_prefix, braced, var_start)) {
        return false;
    }

    if (var_start > token_prefix.size()) {
        return false;
    }

    std::string var_prefix = token_prefix.substr(var_start);
    if (var_prefix.find('}') != std::string::npos) {
        return false;
    }

    if (!is_valid_variable_completion_prefix(var_prefix)) {
        return false;
    }

    std::unordered_set<std::string> candidates;
    if (g_shell && g_shell->get_shell_script_interpreter()) {
        auto names =
            g_shell->get_shell_script_interpreter()->get_variable_manager().get_variable_names();
        candidates.insert(names.begin(), names.end());
    } else {
        const auto& env_vars = cjsh_env::env_vars();
        for (const auto& entry : env_vars) {
            (void)candidates.insert(entry.first);
        }
    }

    static const char* kSpecialVars[] = {"?", "$", "#", "*", "@", "!", "0"};
    for (const char* var_name : kSpecialVars) {
        (void)candidates.insert(var_name);
    }

    if (candidates.empty()) {
        return false;
    }

    std::vector<std::string> ordered_candidates(candidates.begin(), candidates.end());
    auto build_sort_key = [](const std::string& value) {
        return g_completion_case_sensitive ? value
                                           : completion_utils::normalize_for_comparison(value);
    };
    std::sort(ordered_candidates.begin(), ordered_candidates.end(),
              [&](const std::string& lhs, const std::string& rhs) {
                  std::string lhs_key = build_sort_key(lhs);
                  std::string rhs_key = build_sort_key(rhs);
                  if (lhs_key == rhs_key) {
                      return lhs < rhs;
                  }
                  return lhs_key < rhs_key;
              });

    long delete_before = static_cast<long>(var_prefix.size());
    bool added = false;

    for (const auto& name : ordered_candidates) {
        if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv)) {
            break;
        }

        if (!completion_utils::matches_completion_prefix(name, var_prefix)) {
            continue;
        }

        std::string completion_text = name;
        if (braced) {
            completion_text += '}';
        }

        if (!completion_tracker::safe_add_completion_prim_with_source(
                cenv, completion_text.c_str(), nullptr, nullptr, "variable", delete_before, 0)) {
            break;
        }
        added = true;
    }

    return added;
}

CompletionContext detect_completion_context(
    const completion_context::CommandLineContext& command_context) {
    if (!command_context.cursor_in_command_position) {
        return CONTEXT_ARGUMENT;
    }

    const std::string& current = command_context.current_prefix;
    if (current.find('/') == 0 || current.find("./") == 0 || current.find("../") == 0) {
        return CONTEXT_PATH;
    }
    return CONTEXT_COMMAND;
}

bool is_job_control_command(const std::string& token) {
    static const char* kJobCommands[] = {"bg", "fg", "jobs", "jobname", "kill", "disown", "wait"};
    return std::any_of(std::begin(kJobCommands), std::end(kJobCommands),
                       [&](const char* command_name) {
                           return completion_utils::equals_completion_token(token, command_name);
                       });
}

bool add_job_control_argument_completions(ic_completion_env_t* cenv,
                                          const std::vector<std::string>& tokens,
                                          bool ends_with_space) {
    if (tokens.empty()) {
        return false;
    }

    if (!is_job_control_command(tokens.front())) {
        return false;
    }

    if (tokens.size() == 1 && !ends_with_space) {
        return false;
    }

    std::string current_prefix;
    if (!ends_with_space && tokens.size() >= 2) {
        current_prefix = tokens.back();
    }

    auto& job_manager = JobManager::instance();
    job_manager.update_job_statuses();
    auto jobs = job_manager.get_all_jobs();
    if (jobs.empty()) {
        return false;
    }

    long delete_before = static_cast<long>(current_prefix.size());
    bool added = false;

    std::unordered_map<int, std::shared_ptr<JobControlJob>> job_lookup;
    job_lookup.reserve(jobs.size());
    for (const auto& job : jobs) {
        if (job) {
            (void)job_lookup.emplace(job->job_id, job);
        }
    }

    auto build_job_summary = [&](const std::shared_ptr<JobControlJob>& job) {
        const std::string& source = job->has_custom_name() ? job->custom_name : job->command;
        std::string summary = completion_utils::sanitize_job_command_summary(source);
        if (summary.empty()) {
            summary = source.empty() ? std::string("command unavailable") : source;
        }
        return summary;
    };

    auto build_source_label = [&](const std::shared_ptr<JobControlJob>& job,
                                  const std::string& summary_text, const std::string& qualifier) {
        std::string pid_text;
        if (!job->pids.empty()) {
            pid_text = std::to_string(static_cast<long long>(job->pids.front()));
        } else if (job->pgid > 0) {
            pid_text = std::to_string(static_cast<long long>(job->pgid));
        } else {
            pid_text = "unavailable";
        }

        std::string label;
        if (!qualifier.empty()) {
            (void)label.append(qualifier);
            (void)label.append(" · ");
        }
        (void)label.append(summary_text);
        (void)label.append(" · job %");
        (void)label.append(std::to_string(job->job_id));
        (void)label.append(" · pid ");
        (void)label.append(pid_text);
        return label;
    };

    auto matches_prefix = [&](const std::string& candidate) {
        return current_prefix.empty() ||
               completion_utils::matches_completion_prefix(candidate, current_prefix);
    };

    auto add_job_completion = [&](const std::string& token,
                                  const std::shared_ptr<JobControlJob>& job,
                                  const std::string& qualifier) -> bool {
        if (!job) {
            return true;
        }
        if (!matches_prefix(token)) {
            return true;
        }

        std::string insert_text = token;
        if (insert_text.empty() || insert_text.back() != ' ') {
            insert_text.push_back(' ');
        }

        std::string summary_text = build_job_summary(job);
        std::string source_label = build_source_label(job, summary_text, qualifier);
        if (!completion_tracker::safe_add_completion_prim_with_source(
                cenv, insert_text.c_str(), nullptr, nullptr, source_label.c_str(), delete_before,
                0)) {
            return false;
        }
        added = true;
        return true;
    };

    auto add_relative_completion = [&](char marker, const std::string& qualifier,
                                       int job_id) -> bool {
        if (job_id < 0) {
            return true;
        }
        auto it = job_lookup.find(job_id);
        if (it == job_lookup.end()) {
            return true;
        }
        std::string token(1, marker);
        return add_job_completion(token, it->second, qualifier);
    };

    if (!add_relative_completion('+', "current job", job_manager.get_current_job())) {
        return added;
    }
    if (!add_relative_completion('-', "previous job", job_manager.get_previous_job())) {
        return added;
    }

    for (const auto& job : jobs) {
        if (completion_tracker::completion_limit_hit()) {
            break;
        }
        if (ic_stop_completing(cenv)) {
            break;
        }

        std::string completion_text = "%" + std::to_string(job->job_id);

        if (!current_prefix.empty() &&
            !completion_utils::matches_completion_prefix(completion_text, current_prefix)) {
            continue;
        }

        if (!add_job_completion(completion_text, job, "")) {
            return added;
        }

        if (completion_tracker::completion_limit_hit() || ic_stop_completing(cenv)) {
            break;
        }
    }

    return added;
}

struct ArgumentCompletionContext {
    std::string current_prefix;
    size_t argument_index;
};

bool build_argument_completion_context(const std::vector<std::string>& tokens, bool ends_with_space,
                                       ArgumentCompletionContext& context) {
    if (tokens.empty()) {
        return false;
    }

    if (ends_with_space) {
        context.current_prefix.clear();
        context.argument_index = tokens.size();
        return true;
    }

    context.current_prefix = tokens.back();
    context.argument_index = tokens.size() - 1;
    return true;
}

bool add_hook_type_completions(ic_completion_env_t* cenv, const std::string& prefix,
                               size_t prefix_len) {
    const auto& descriptors = get_hook_type_descriptors();
    std::vector<std::string> hook_types;
    hook_types.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        if (descriptor.name != nullptr) {
            (void)hook_types.emplace_back(descriptor.name);
        }
    }

    if (hook_types.empty()) {
        return false;
    }

    process_command_candidates(cenv, hook_types, prefix, prefix_len, "hook type",
                               [](const std::string& value) { return value; });
    return ic_has_completions(cenv);
}

bool add_builtin_argument_completions(ic_completion_env_t* cenv,
                                      const std::vector<std::string>& tokens,
                                      bool ends_with_space) {
    if (tokens.empty() || cenv == nullptr) {
        return false;
    }
    if (ic_stop_completing(cenv) || completion_tracker::completion_limit_hit()) {
        return false;
    }

    ArgumentCompletionContext context;
    if (!build_argument_completion_context(tokens, ends_with_space, context)) {
        return false;
    }

    const std::string& command = tokens[0];
    size_t prefix_len = context.current_prefix.size();

    auto matches_command = [&](const char* name) {
        return completion_utils::equals_completion_token(command, name);
    };

    if (matches_command("builtin")) {
        if (context.argument_index != 1 || g_shell == nullptr ||
            g_shell->get_built_ins() == nullptr) {
            return false;
        }
        auto builtin_cmds = g_shell->get_built_ins()->get_builtin_commands();
        add_builtin_command_candidates(cenv, builtin_cmds, context.current_prefix, prefix_len);
        return ic_has_completions(cenv);
    }

    if (matches_command("alias") || matches_command("unalias")) {
        if (context.argument_index < 1 || g_shell == nullptr) {
            return false;
        }
        if (context.current_prefix.find('=') != std::string::npos) {
            return false;
        }
        const auto& alias_map = g_shell->get_aliases();
        auto alias_names = collect_map_keys(alias_map);
        auto alias_source_provider = make_map_source_provider(&alias_map);
        process_command_candidates(
            cenv, alias_names, context.current_prefix, prefix_len, "alias",
            [](const std::string& value) { return value; },
            std::function<bool(const std::string&)>{}, alias_source_provider);
        return ic_has_completions(cenv);
    }

    if (matches_command("abbr") || matches_command("abbreviate") || matches_command("unabbr") ||
        matches_command("unabbreviate")) {
        if (context.argument_index < 1 || g_shell == nullptr) {
            return false;
        }
        if (context.current_prefix.find('=') != std::string::npos) {
            return false;
        }
        const auto& abbr_map = g_shell->get_abbreviations();
        auto abbr_names = collect_map_keys(abbr_map);
        auto abbr_source_provider = make_map_source_provider(&abbr_map);
        process_command_candidates(
            cenv, abbr_names, context.current_prefix, prefix_len, "abbreviation",
            [](const std::string& value) { return value; },
            std::function<bool(const std::string&)>{}, abbr_source_provider);
        return ic_has_completions(cenv);
    }

    if (matches_command("type") || matches_command("which")) {
        if (context.argument_index < 1) {
            return false;
        }

        bool options_ended = false;
        if (context.argument_index > 1) {
            for (size_t index = 1; index < context.argument_index; ++index) {
                if (completion_utils::equals_completion_token(tokens[index], "--")) {
                    options_ended = true;
                    break;
                }
            }
        }

        if (!options_ended && !context.current_prefix.empty() && context.current_prefix[0] == '-') {
            return false;
        }

        auto sources = collect_command_completion_sources();
        add_command_name_completions(cenv, sources, context.current_prefix, prefix_len, false);

        return ic_has_completions(cenv);
    }

    if (matches_command("hook")) {
        if (tokens.size() < 2) {
            return false;
        }
        const std::string& subcommand = tokens[1];
        bool is_add = completion_utils::equals_completion_token(subcommand, "add");
        bool is_remove = completion_utils::equals_completion_token(subcommand, "remove");
        bool is_list = completion_utils::equals_completion_token(subcommand, "list");
        bool is_clear = completion_utils::equals_completion_token(subcommand, "clear");

        if ((is_add || is_remove || is_list || is_clear) && context.argument_index == 2) {
            return add_hook_type_completions(cenv, context.current_prefix, prefix_len);
        }

        if ((is_add || is_remove) && context.argument_index == 3 && g_shell != nullptr) {
            std::vector<std::string> candidates;
            if (tokens.size() >= 3) {
                auto hook_type = parse_hook_type(tokens[2]);
                if (hook_type.has_value()) {
                    candidates = g_shell->get_hooks(*hook_type);
                }
            }

            if (candidates.empty() && g_shell->get_shell_script_interpreter() != nullptr) {
                candidates = g_shell->get_shell_script_interpreter()->get_function_names();
            }

            if (!candidates.empty()) {
                process_command_candidates(cenv, candidates, context.current_prefix, prefix_len,
                                           "function",
                                           [](const std::string& value) { return value; });
            }
            return ic_has_completions(cenv);
        }
    }

    if (matches_command("cjshopt") &&
        (tokens.size() >= 2 && completion_utils::equals_completion_token(tokens[1], "style_def") &&
         context.argument_index == 2 &&
         (context.current_prefix.empty() || context.current_prefix[0] != '-'))) {
        const auto& styles = token_constants::default_styles();
        std::vector<std::string> style_tokens;
        style_tokens.reserve(styles.size() + 1);
        style_tokens.push_back("preview");
        for (const auto& entry : styles) {
            style_tokens.push_back(entry.first);
        }
        process_command_candidates(cenv, style_tokens, context.current_prefix, prefix_len,
                                   "style token", [](const std::string& value) { return value; });
        return ic_has_completions(cenv);
    }

    return false;
}

void add_command_token_completions(ic_completion_env_t* cenv, const std::string& decoded_prefix,
                                   std::size_t raw_prefix_length) {
    if (ic_stop_completing(cenv) || completion_tracker::completion_limit_hit()) {
        return;
    }

    auto sources = collect_command_completion_sources();
    add_command_name_completions(cenv, sources, decoded_prefix, raw_prefix_length);
}

}  // namespace

void cjsh_command_completer(ic_completion_env_t* cenv, const char* prefix) {
    std::string prefix_str;
    size_t prefix_len = 0;
    if (!prepare_prefix_state(cenv, prefix, prefix_str, prefix_len)) {
        return;
    }

    add_command_token_completions(cenv, prefix_str, prefix_len);
}

bool looks_like_file_path(const std::string& str) {
    if (str.empty()) {
        return false;
    }

    if (str[0] == '/' || str.rfind("./", 0) == 0 || str.rfind("../", 0) == 0 ||
        str.rfind("~/", 0) == 0 || str.find('/') != std::string::npos) {
        return true;
    }

    size_t dot_pos = str.rfind('.');
    if (dot_pos != std::string::npos && dot_pos > 0 && dot_pos < str.length() - 1) {
        std::string extension = str.substr(dot_pos + 1);

        static const std::unordered_set<std::string> file_extensions = {
            "txt",  "log",  "conf", "config", "json", "xml",  "yaml", "yml", "cpp",
            "c",    "h",    "hpp",  "py",     "js",   "ts",   "java", "sh",  "bash",
            "md",   "html", "css",  "sql",    "tar",  "gz",   "zip",  "pdf", "doc",
            "docx", "xls",  "xlsx", "png",    "jpg",  "jpeg", "gif",  "mp3", "mp4"};

        std::string ext_lower = string_utils::to_lower_copy(extension);

        if (file_extensions.find(ext_lower) != file_extensions.end()) {
            return true;
        }
    }

    return false;
}

namespace {

enum class HistoryCompletionGroup : std::uint8_t {
    ALL,
    SUCCESSFUL,
    REMAINING,
};

struct HistoryMatch {
    std::string command;
    bool has_exit_code;
    int exit_code;
    long long timestamp = 0;
    long long frequency = 1;
};

struct HistoryCompletionBatch {
    size_t prefix_len{};
    std::vector<HistoryMatch> matches;
};

bool collect_history_completion_matches(ic_completion_env_t* cenv, const char* prefix,
                                        HistoryCompletionBatch& batch, bool rank_by_usage = false) {
    std::string prefix_str;
    size_t prefix_len = 0;
    if (!prepare_prefix_state(cenv, prefix, prefix_str, prefix_len)) {
        return false;
    }

    std::ifstream history_file(cjsh_filesystem::g_cjsh_history_path());
    if (!history_file.is_open()) {
        return false;
    }

    batch.prefix_len = prefix_len;
    batch.matches.clear();
    batch.matches.reserve(50);

    std::string line;
    line.reserve(256);
    std::string decoded_line;
    decoded_line.reserve(256);

    std::string header_line;
    const bool directory_aware = ic_history_directory_is_enabled();

    while (std::getline(history_file, line) && (rank_by_usage || batch.matches.size() < 50)) {
        if (line.empty()) {
            continue;
        }

        if (line[0] == '#') {
            header_line = line;
            continue;
        }

        if (!decode_history_command_line(line, decoded_line)) {
            decoded_line = line;
        }
        const std::string& entry_text = decoded_line;
        const bool should_match = entry_text != prefix_str &&
                                  (prefix_len == 0 || completion_utils::matches_completion_prefix(
                                                          entry_text, prefix_str));
        if (!should_match) {
            header_line.clear();
            continue;
        }

        // Metadata only affects eligible matches. Consume it with this command,
        // including when the command is filtered out, so it cannot leak forward.
        int last_exit_code = 0;
        bool has_last_exit_code = false;
        long long last_timestamp = 0;
        long long last_frequency = 1;
        std::string directory;
        // Parse fields as views into the saved header. Ranked history can visit
        // every record on each completion, so avoid a stream and strings per field.
        const std::string_view header(header_line);
        size_t token_start = header.find_first_not_of(" \t\r\n\v\f", 1);
        while (token_start != std::string_view::npos) {
            size_t token_end = header.find_first_of(" \t\r\n\v\f", token_start);
            if (token_end == std::string_view::npos) {
                token_end = header.size();
            }
            const std::string_view token = header.substr(token_start, token_end - token_start);
            const size_t equals_pos = token.find('=');
            if (equals_pos != std::string_view::npos && equals_pos != 0 &&
                equals_pos + 1 < token.size()) {
                const std::string_view key = token.substr(0, equals_pos);
                const std::string_view value = token.substr(equals_pos + 1);
                if (key == "code" || key == "exit_code") {
                    char* endptr = nullptr;
                    const long exit_ll = std::strtol(value.data(), &endptr, 10);
                    if (endptr != value.data() && endptr == value.data() + value.size()) {
                        last_exit_code = static_cast<int>(exit_ll);
                        has_last_exit_code = true;
                    }
                } else if (directory_aware && key == "cwd") {
                    directory.clear();
                    for (size_t i = 0; i < value.size(); ++i) {
                        if (value[i] == '%' && i + 2 < value.size()) {
                            unsigned int byte = 0;
                            const auto result = std::from_chars(value.data() + i + 1,
                                                                value.data() + i + 3, byte, 16);
                            if (result.ec == std::errc{} && result.ptr == value.data() + i + 3) {
                                directory.push_back(static_cast<char>(byte));
                                i += 2;
                                continue;
                            }
                        }
                        directory.push_back(value[i]);
                    }
                } else if (rank_by_usage && (key == "timestamp" || key == "frequency")) {
                    long long parsed = 0;
                    const auto result =
                        std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                        parsed >= 0) {
                        if (key == "timestamp") {
                            last_timestamp = parsed;
                        } else if (parsed > 0) {
                            last_frequency = parsed;
                        }
                    }
                }
            }
            token_start = header.find_first_not_of(" \t\r\n\v\f", token_end);
        }
        header_line.clear();

        if ((directory_aware && !ic_history_matches_directory(directory.c_str())) ||
            (has_last_exit_code && last_exit_code == kHistoryCompletionHiddenExitCode) ||
            (!rank_by_usage && looks_like_file_path(entry_text)) ||
            string_utils::trim_ascii_whitespace_copy(entry_text).empty()) {
            continue;
        }

        batch.matches.push_back(HistoryMatch{entry_text, has_last_exit_code, last_exit_code,
                                             last_timestamp, last_frequency});
    }

    if (rank_by_usage) {
        // History is stored oldest-first. Prefer later records when metadata ties or is absent.
        std::reverse(batch.matches.begin(), batch.matches.end());
        std::stable_sort(batch.matches.begin(), batch.matches.end(),
                         [](const HistoryMatch& lhs, const HistoryMatch& rhs) {
                             if (lhs.timestamp != rhs.timestamp) {
                                 return lhs.timestamp > rhs.timestamp;
                             }
                             return lhs.frequency > rhs.frequency;
                         });
        // Deduplicate before applying the suggestion cap so repeats cannot crowd out other
        // commands.
        std::unordered_set<std::string> seen;
        batch.matches.erase(
            std::remove_if(batch.matches.begin(), batch.matches.end(),
                           [&](const HistoryMatch& match) {
                               return !seen.insert(string_utils::trim_right_ascii_whitespace_copy(
                                                       match.command))
                                           .second;
                           }),
            batch.matches.end());
    }

    return true;
}

bool history_match_is_in_group(const HistoryMatch& match, HistoryCompletionGroup group) {
    const bool successful = match.has_exit_code && match.exit_code == 0;
    switch (group) {
        case HistoryCompletionGroup::ALL:
            return true;
        case HistoryCompletionGroup::SUCCESSFUL:
            return successful;
        case HistoryCompletionGroup::REMAINING:
            return !successful;
    }
    return false;
}

void add_history_completion_matches(ic_completion_env_t* cenv, const HistoryCompletionBatch& batch,
                                    HistoryCompletionGroup group, size_t max_suggestions = 15) {
    size_t considered = 0;

    for (const auto& match : batch.matches) {
        if (considered++ >= max_suggestions) {
            return;
        }
        if (!history_match_is_in_group(match, group)) {
            continue;
        }
        if (completion_tracker::completion_limit_hit()) {
            return;
        }

        const std::string& completion = match.command;
        long delete_before = static_cast<long>(batch.prefix_len);

        const bool display_exit_code = match.has_exit_code;
        std::string source_label =
            display_exit_code ? "history: " + std::to_string(match.exit_code) : "history";
        if (!completion_tracker::safe_add_completion_prim_with_source(
                cenv, completion.c_str(), nullptr, nullptr, source_label.c_str(), delete_before,
                0)) {
            return;
        }
        if (ic_stop_completing(cenv)) {
            return;
        }
    }
}

}  // namespace

void cjsh_history_completer(ic_completion_env_t* cenv, const char* prefix) {
    HistoryCompletionBatch batch;
    if (collect_history_completion_matches(cenv, prefix, batch)) {
        add_history_completion_matches(cenv, batch, HistoryCompletionGroup::ALL);
    }
}

bool should_complete_directories_only(const std::string& prefix) {
    std::string command;
    size_t first_space = prefix.find(' ');

    if (first_space != std::string::npos) {
        command = prefix.substr(0, first_space);
    } else {
        return false;
    }

    static const std::unordered_set<std::string> directory_only_commands = {"cd", "ls", "dir",
                                                                            "rmdir"};
    if (g_completion_case_sensitive) {
        return directory_only_commands.find(command) != directory_only_commands.end();
    }

    std::string lowered_command = string_utils::to_lower_copy(command);
    return directory_only_commands.find(lowered_command) != directory_only_commands.end();
}

namespace {

bool history_match_duplicates_file_completion(const HistoryMatch& match,
                                              const std::string& completion_prefix) {
    namespace fs = std::filesystem;

    const std::string command = string_utils::trim_right_ascii_whitespace_copy(match.command);
    const size_t last_space = completion_utils::find_last_unquoted_space(completion_prefix);
    const size_t path_start = last_space == std::string::npos ? 0 : last_space + 1;

    if (command.size() <= path_start ||
        command.compare(0, path_start, completion_prefix, 0, path_start) != 0) {
        return false;
    }

    const std::string raw_candidate = command.substr(path_start);
    if (completion_utils::find_last_unquoted_space(raw_candidate) != std::string::npos) {
        return false;
    }

    const std::string candidate_path_text = completion_utils::unquote_path(raw_candidate);
    const std::string current_path_text =
        completion_utils::unquote_path(completion_prefix.substr(path_start));
    if (candidate_path_text.empty() ||
        !completion_utils::matches_completion_prefix(candidate_path_text, current_path_text)) {
        return false;
    }

    fs::path completion_dir;
    std::string match_prefix;
    const bool treat_as_directory = current_path_text.empty() || current_path_text.back() == '/';
    determine_directory_target(current_path_text, treat_as_directory, completion_dir, match_prefix);

    const fs::path candidate_path(candidate_path_text);
    fs::path candidate_dir = candidate_path.parent_path();
    if (candidate_dir.empty()) {
        candidate_dir = ".";
    }
    if (candidate_dir.lexically_normal() != completion_dir.lexically_normal()) {
        return false;
    }

    const std::string filename = candidate_path.filename().string();
    if (filename.empty() ||
        (!match_prefix.empty() &&
         !completion_utils::matches_completion_prefix(filename, match_prefix)) ||
        (match_prefix.empty() && filename[0] == '.')) {
        return false;
    }

    std::error_code ec;
    fs::directory_entry entry(candidate_path, ec);
    if (ec) {
        return false;
    }
    const fs::file_status status = entry.symlink_status(ec);
    if (ec || status.type() == fs::file_type::not_found) {
        return false;
    }

    if (should_complete_directories_only(completion_prefix)) {
        ec.clear();
        if (!entry.is_directory(ec) || ec) {
            return false;
        }
    }

    const bool has_command_prefix = path_start > 0;
    const bool restrict_to_executables =
        !has_command_prefix &&
        completion_utils::starts_with_case_sensitive(current_path_text, "./");
    return !restrict_to_executables || is_executable_or_script_entry(entry);
}

void remove_history_matches_duplicated_by_files(HistoryCompletionBatch& batch,
                                                const std::string& completion_prefix) {
    batch.matches.erase(std::remove_if(batch.matches.begin(), batch.matches.end(),
                                       [&](const HistoryMatch& match) {
                                           return history_match_duplicates_file_completion(
                                               match, completion_prefix);
                                       }),
                        batch.matches.end());
}

}  // namespace

void cjsh_filename_completer(ic_completion_env_t* cenv, const char* prefix) {
    if (ic_stop_completing(cenv)) {
        return;
    }

    if (completion_tracker::completion_limit_hit()) {
        return;
    }

    std::string prefix_str(prefix);
    bool directories_only = should_complete_directories_only(prefix_str);

    size_t last_space = completion_utils::find_last_unquoted_space(prefix_str);

    bool has_tilde = false;
    bool has_dash = false;
    std::string prefix_before;
    std::string special_part;

    auto complete_special_prefix = [&](const std::string& dir_to_complete,
                                       bool treat_as_directory) {
        namespace fs = std::filesystem;
        fs::path dir_path;
        std::string match_prefix;
        determine_directory_target(dir_to_complete, treat_as_directory, dir_path, match_prefix);

        try {
            if ((fs::exists(dir_path) && fs::is_directory(dir_path)) &&
                (!iterate_directory_entries(cenv, dir_path, match_prefix, false, false))) {
                return false;
            }

        } catch (const std::exception&) {
            // Best-effort completion: ignore filesystem errors.
        }

        return true;
    };

    if (last_space != std::string::npos) {
        prefix_before = prefix_str.substr(0, last_space + 1);

        if (last_space + 1 < prefix_str.length()) {
            special_part = prefix_str.substr(last_space + 1);

            if (!special_part.empty() && special_part[0] == '~') {
                has_tilde = true;
            } else if (!special_part.empty() && special_part[0] == '-' &&
                       (special_part.length() == 1 || special_part[1] == '/')) {
                has_dash = true;
            }
        } else {
            special_part.clear();
        }
    } else if (!prefix_str.empty() && prefix_str[0] == '~') {
        has_tilde = true;
        special_part = prefix_str;
    } else if (!prefix_str.empty() && prefix_str[0] == '-' &&
               (prefix_str.length() == 1 || prefix_str[1] == '/')) {
        has_dash = true;
        special_part = prefix_str;
    }

    if ((has_tilde && (special_part.length() == 1 || special_part[1] == '/')) ||
        (has_dash && (special_part.length() == 1 || special_part[1] == '/'))) {
        std::string unquoted_special = completion_utils::unquote_path(special_part);
        const std::string cwd = cjsh_filesystem::safe_current_directory();
        const std::string previous_directory = g_shell ? g_shell->get_previous_directory() : "";

        std::filesystem::path expanded =
            cjsh_filesystem::expand_shell_path_token(unquoted_special, cwd, previous_directory);
        if (expanded.empty()) {
            return;
        }

        bool treat_as_directory = !unquoted_special.empty() && unquoted_special.back() == '/';
        if (!complete_special_prefix(expanded.string(), treat_as_directory)) {
            return;
        }
        return;
    }

    const bool has_command_prefix = !prefix_before.empty();
    std::string raw_path_input = has_command_prefix ? special_part : prefix_str;
    std::string path_to_check = completion_utils::unquote_path(raw_path_input);
    bool restrict_to_executables =
        !has_command_prefix && completion_utils::starts_with_case_sensitive(path_to_check, "./");

    if (!ic_stop_completing(cenv) && !path_to_check.empty() && path_to_check.back() == '/') {
        namespace fs = std::filesystem;
        fs::path dir_path(path_to_check);
        try {
            if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
                bool had_completions_before = ic_has_completions(cenv);
                if (!iterate_directory_entries(cenv, dir_path, "", directories_only, false,
                                               restrict_to_executables, restrict_to_executables)) {
                    return;
                }

                if ((directories_only && !ic_has_completions(cenv) && !had_completions_before) &&
                    (!iterate_directory_entries(cenv, dir_path, "", false, false,
                                                restrict_to_executables,
                                                restrict_to_executables))) {
                    return;
                }
            }
        } catch (const std::exception& e) {
            // Best-effort completion: ignore filesystem errors.
        }
        return;
    }

    const std::string& path_to_complete = path_to_check;

    namespace fs = std::filesystem;
    fs::path dir_path;
    std::string match_prefix;
    bool treat_as_directory = path_to_complete.empty() || path_to_complete.back() == '/';
    determine_directory_target(path_to_complete, treat_as_directory, dir_path, match_prefix);

    try {
        if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
            if (directories_only) {
                bool had_completions_before = ic_has_completions(cenv);
                if (!iterate_directory_entries(cenv, dir_path, match_prefix, true, true,
                                               restrict_to_executables, restrict_to_executables)) {
                    return;
                }

                if ((!ic_has_completions(cenv) && !had_completions_before &&
                     match_prefix.empty()) &&
                    (!iterate_directory_entries(cenv, dir_path, "", false, true,
                                                restrict_to_executables,
                                                restrict_to_executables))) {
                    return;
                }

            } else {
                if (!iterate_directory_entries(cenv, dir_path, match_prefix, false, true,
                                               restrict_to_executables, restrict_to_executables)) {
                    return;
                }
            }
        }
    } catch (const std::exception&) {
        // Best-effort completion: ignore filesystem errors.
    }
}

void cjsh_default_completer(ic_completion_env_t* cenv, const char* prefix) {
    if (ic_stop_completing(cenv)) {
        return;
    }

    if (!ic_completion_is_hint(cenv)) {
        cjsh_filesystem::reset_interactive_path_cache();
    }
    const cjsh_filesystem::ScopedInteractivePathLookup path_lookup;

    const char* effective_prefix = (prefix != nullptr) ? prefix : "";
    std::string completion_scope_prefix = extract_completion_scope_prefix(effective_prefix);
    completion_context::CommandLineContext command_context;
    bool context_reusable = false;
    long raw_cursor = 0;
    const char* raw_input = ic_completion_input(cenv, &raw_cursor);
    if (raw_input != nullptr && raw_cursor >= 0) {
        command_context =
            completion_context::parse(raw_input, static_cast<std::size_t>(raw_cursor));
        if (command_context.cursor_in_assignment_lhs ||
            command_context.cursor_before_existing_word) {
            return;
        }
        context_reusable = std::string_view(raw_input).substr(0, command_context.cursor) ==
                           completion_scope_prefix;
    }

    if (!context_reusable) {
        command_context = completion_context::parse(completion_scope_prefix);
    }
    if (cursor_is_inside_known_command(raw_input, raw_cursor, command_context)) {
        return;
    }
    std::string active_prefix = command_context.segment_prefix;
    const char* current_line_prefix = active_prefix.c_str();

    completion_tracker::completion_session_begin(cenv, effective_prefix);

    const char* full_input = raw_input != nullptr ? raw_input : effective_prefix;
    if (string_utils::trim_ascii_whitespace_copy(full_input).empty()) {
        HistoryCompletionBatch history_matches;
        if (config::history_enabled &&
            collect_history_completion_matches(cenv, "", history_matches, true)) {
            add_history_completion_matches(cenv, history_matches, HistoryCompletionGroup::ALL,
                                           static_cast<size_t>(get_completion_max_results()));
        }
        completion_tracker::completion_session_end();
        return;
    }

    CompletionContext context = detect_completion_context(command_context);

    switch (context) {
        case CONTEXT_COMMAND: {
            const std::string& command_raw_prefix = command_context.current_raw_prefix;
            const std::string& command_prefix = command_context.current_prefix;
            HistoryCompletionBatch history_matches;
            const bool has_history_matches = collect_history_completion_matches(
                cenv, command_raw_prefix.c_str(), history_matches);
            if (has_history_matches) {
                remove_history_matches_duplicated_by_files(history_matches, command_raw_prefix);
            }

            (void)add_variable_completions(cenv, command_raw_prefix);
            if (ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }
            if (has_history_matches) {
                add_history_completion_matches(cenv, history_matches,
                                               HistoryCompletionGroup::SUCCESSFUL);
                if (ic_stop_completing(cenv)) {
                    completion_tracker::completion_session_end();
                    return;
                }
            }
            cjsh_filename_completer(cenv, command_raw_prefix.c_str());
            if (ic_has_completions(cenv) && ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }

            add_command_token_completions(cenv, command_prefix, command_raw_prefix.size());
            if (ic_has_completions(cenv) && ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }

            if (has_history_matches) {
                add_history_completion_matches(cenv, history_matches,
                                               HistoryCompletionGroup::REMAINING);
            }
            if (ic_has_completions(cenv) && ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }

            break;
        }

        case CONTEXT_PATH: {
            (void)add_variable_completions(cenv, command_context.current_raw_prefix);
            if (ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }
            HistoryCompletionBatch history_matches;
            const bool has_history_matches =
                config::history_enabled &&
                collect_history_completion_matches(cenv, command_context.current_raw_prefix.c_str(),
                                                   history_matches);
            if (has_history_matches) {
                remove_history_matches_duplicated_by_files(history_matches,
                                                           command_context.current_raw_prefix);
            }
            if (has_history_matches) {
                add_history_completion_matches(cenv, history_matches,
                                               HistoryCompletionGroup::SUCCESSFUL);
                if (ic_stop_completing(cenv)) {
                    completion_tracker::completion_session_end();
                    return;
                }
            }
            cjsh_filename_completer(cenv, command_context.current_raw_prefix.c_str());
            if (ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }
            if (has_history_matches) {
                add_history_completion_matches(cenv, history_matches,
                                               HistoryCompletionGroup::REMAINING);
            }
            break;
        }

        case CONTEXT_ARGUMENT: {
            std::string prefix_str(current_line_prefix);
            std::vector<std::string> tokens;
            tokens.reserve(command_context.words.size());
            for (const auto& word : command_context.words) {
                tokens.push_back(word.text);
            }

            (void)add_variable_completions(cenv, prefix_str);
            if (ic_stop_completing(cenv)) {
                completion_tracker::completion_session_end();
                return;
            }

            bool ends_with_space = command_context.at_word_boundary;

            (void)add_job_control_argument_completions(cenv, tokens, ends_with_space);

            (void)add_builtin_argument_completions(cenv, tokens, ends_with_space);

            if (!tokens.empty()) {
                handle_external_sub_completions(cenv, command_context);
            }

            (void)add_split_unknown_command_completions(cenv, tokens, ends_with_space, prefix_str);

            if (!tokens.empty() && completion_utils::equals_completion_token(tokens[0], "cd")) {
                cjsh_filename_completer(cenv, current_line_prefix);
            } else {
                HistoryCompletionBatch history_matches;
                const bool has_history_matches =
                    config::history_enabled &&
                    collect_history_completion_matches(cenv, current_line_prefix, history_matches);
                if (has_history_matches) {
                    remove_history_matches_duplicated_by_files(history_matches,
                                                               current_line_prefix);
                }
                if (has_history_matches) {
                    add_history_completion_matches(cenv, history_matches,
                                                   HistoryCompletionGroup::SUCCESSFUL);
                    if (ic_stop_completing(cenv)) {
                        completion_tracker::completion_session_end();
                        return;
                    }
                }
                cjsh_filename_completer(cenv, current_line_prefix);
                if (ic_stop_completing(cenv)) {
                    completion_tracker::completion_session_end();
                    return;
                }
                if (has_history_matches) {
                    add_history_completion_matches(cenv, history_matches,
                                                   HistoryCompletionGroup::REMAINING);
                }
            }
            break;
        }
    }

    completion_tracker::completion_session_end();
}

void initialize_completion_system() {
    if (config::completions_enabled) {
        ic_set_default_completer(cjsh_default_completer, nullptr);
    } else {
        ic_set_default_completer(nullptr, nullptr);
        (void)ic_enable_completion_preview(false);
        (void)ic_enable_hint(false);
        (void)ic_enable_auto_tab(false);
        (void)ic_enable_inline_help(false);
    }
    (void)ic_enable_spell_correct(g_completion_spell_correction_enabled);
    (void)ic_enable_spell_correct_on_enter(g_completion_spell_correction_on_enter_enabled);
    if (!completion_history::enforce_history_limit(nullptr)) {
        print_error(
            {ErrorType::RUNTIME_ERROR,
             ErrorSeverity::WARNING,
             "completions",
             "failed to enforce history limit; history file may exceed the configured size.",
             {"Check disk permissions or trim the history file manually."}});
    }
}

void set_completion_case_sensitive(bool case_sensitive) {
    g_completion_case_sensitive = case_sensitive;
}

bool is_completion_case_sensitive() {
    return g_completion_case_sensitive;
}

void set_completion_spell_correction_enabled(bool enabled) {
    g_completion_spell_correction_enabled = enabled;
    (void)ic_enable_spell_correct(enabled);
}

bool is_completion_spell_correction_enabled() {
    return g_completion_spell_correction_enabled;
}

void set_completion_spell_correction_on_enter_enabled(bool enabled) {
    g_completion_spell_correction_on_enter_enabled = enabled;
    (void)ic_enable_spell_correct_on_enter(enabled);
}

bool is_completion_spell_correction_on_enter_enabled() {
    return g_completion_spell_correction_on_enter_enabled;
}

bool set_completion_max_results(long max_results, std::string* error_message) {
    return completion_tracker::set_completion_max_results(max_results, error_message);
}

long get_completion_max_results() {
    return completion_tracker::get_completion_max_results();
}

long get_completion_default_max_results() {
    return completion_tracker::get_completion_default_max_results();
}

long get_completion_min_allowed_results() {
    return completion_tracker::get_completion_min_allowed_results();
}

bool set_history_max_entries(long max_entries, std::string* error_message) {
    return completion_history::set_history_max_entries(max_entries, error_message);
}

long get_history_max_entries() {
    return completion_history::get_history_max_entries();
}

long get_history_default_history_limit() {
    return completion_history::get_history_default_history_limit();
}

long get_history_min_history_limit() {
    return completion_history::get_history_min_history_limit();
}
