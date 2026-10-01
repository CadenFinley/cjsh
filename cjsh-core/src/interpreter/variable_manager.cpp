/*
  variable_manager.cpp

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

#include "variable_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "arithmetic_evaluator.h"
#include "flags.h"
#include "numeric_utils.h"
#include "parameter_utils.h"
#include "parser_utils.h"
#include "readonly_command.h"
#include "shell.h"
#include "shell_env.h"

namespace {

bool is_array_join_index(const std::string& index) {
    return index == "@" || index == "*";
}

void assign_string_value(std::string& target, const std::string& value, bool append) {
    if (append) {
        target += value;
    } else {
        target = value;
    }
}

}  // namespace

void VariableManager::push_scope() {
    (void)local_variable_stack.emplace_back();
    (void)local_array_stack.emplace_back();
    (void)local_associative_array_stack.emplace_back();
    (void)local_nameref_stack.emplace_back();
    (void)exported_locals_stack.emplace_back();
    (void)saved_env_stack.emplace_back();
}

void VariableManager::pop_scope() {
    if (!saved_env_stack.empty()) {
        for (const auto& [var_name, old_value] : saved_env_stack.back()) {
            if (old_value.empty()) {
                (void)unsetenv(var_name.c_str());
            } else {
                (void)setenv(var_name.c_str(), old_value.c_str(), 1);
            }
        }
        saved_env_stack.pop_back();
    }

    if (!exported_locals_stack.empty()) {
        exported_locals_stack.pop_back();
    }

    if (!local_variable_stack.empty()) {
        local_variable_stack.pop_back();
    }
    if (!local_array_stack.empty()) {
        local_array_stack.pop_back();
    }
    if (!local_associative_array_stack.empty()) {
        local_associative_array_stack.pop_back();
    }
    if (!local_nameref_stack.empty()) {
        local_nameref_stack.pop_back();
    }
}

void VariableManager::set_local_variable(const std::string& name, const std::string& value) {
    if (local_variable_stack.empty()) {
        (void)assign_variable(name, value, false);
        return;
    }

    std::string resolved_name = resolve_nameref_reference(name);
    if (resolved_name != name) {
        (void)assign_variable(resolved_name, value, false);
        return;
    }

    ParsedArrayReference parsed;
    if (parse_array_reference(name, parsed) && parsed.has_index) {
        (void)assign_array_element_value(parsed.name, parsed.index, value, false, true);
        return;
    }

    if (has_local_array_binding(name)) {
        (void)assign_scalar_value(name, value, false, true);
        return;
    }
    if (has_local_associative_array_binding(name)) {
        (void)assign_scalar_value(name, value, false, true);
        return;
    }

    local_variable_stack.back()[name] = value;
}

void VariableManager::set_environment_variable(const std::string& name, const std::string& value) {
    if (g_shell) {
        auto& env_vars = cjsh_env::env_vars();
        env_vars[name] = value;
        cjsh_env::mirror_set_to_process_env(name, value);

        (void)global_array_variables.erase(name);
        (void)global_associative_array_variables.erase(name);
        (void)global_nameref_variables.erase(name);

        if (auto* parser = g_shell->get_parser()) {
            parser->set_env_var(name, value);
        }
    }
}

bool VariableManager::assign_variable(const std::string& target, const std::string& value,
                                      bool append) {
    std::string resolved_target = resolve_nameref_reference(target);
    ParsedArrayReference parsed;
    if (!parse_array_reference(resolved_target, parsed)) {
        return false;
    }

    bool local_scope = should_assign_to_local_scope(parsed.name);
    if (parsed.has_index) {
        return assign_array_element_value(parsed.name, parsed.index, value, append, local_scope);
    }

    return assign_scalar_value(parsed.name, value, append, local_scope);
}

bool VariableManager::assign_global_variable(const std::string& target, const std::string& value,
                                             bool append) {
    std::string resolved_target = resolve_nameref_reference(target);
    ParsedArrayReference parsed;
    if (!parse_array_reference(resolved_target, parsed)) {
        return false;
    }

    if (parsed.has_index) {
        return assign_array_element_value(parsed.name, parsed.index, value, append, false);
    }

    return assign_scalar_value(parsed.name, value, append, false);
}

bool VariableManager::assign_array_literal(const std::string& name,
                                           const std::vector<std::string>& words, bool append) {
    return assign_array_literal_impl(name, words, append, false);
}

bool VariableManager::assign_global_array_literal(const std::string& name,
                                                  const std::vector<std::string>& words,
                                                  bool append) {
    return assign_array_literal_impl(name, words, append, true);
}

bool VariableManager::assign_array_literal_impl(const std::string& name,
                                                const std::vector<std::string>& words, bool append,
                                                bool force_global) {
    std::string resolved_name = resolve_nameref_reference(name);
    if (resolved_name != name) {
        return assign_array_literal_impl(resolved_name, words, append, force_global);
    }
    if (!is_valid_identifier(name)) {
        return false;
    }
    if (is_associative_array(name)) {
        return assign_associative_literal_impl(name, words, append, force_global);
    }

    bool local_scope = !force_global && should_assign_to_local_scope(name);
    IndexedArray* target_array = nullptr;

    if (local_scope) {
        if (local_array_stack.empty()) {
            return false;
        }

        auto& arrays = local_array_stack.back();
        auto& scalars = local_variable_stack.back();
        auto scalar_it = scalars.find(name);

        auto [array_it, inserted] = arrays.emplace(name, IndexedArray{});
        target_array = &array_it->second;

        if (append) {
            if (inserted && scalar_it != scalars.end()) {
                (void)target_array->emplace(0, scalar_it->second);
                (void)scalars.erase(scalar_it);
            }
        } else {
            target_array->clear();
            if (scalar_it != scalars.end()) {
                (void)scalars.erase(scalar_it);
            }
        }
    } else {
        auto [array_it, inserted] = global_array_variables.emplace(name, IndexedArray{});
        target_array = &array_it->second;

        if (append) {
            if (inserted && has_global_scalar_binding(name)) {
                (void)target_array->emplace(0, get_global_scalar_value(name));
                remove_global_scalar_binding(name);
            }
        } else {
            target_array->clear();
            remove_global_scalar_binding(name);
        }
    }

    return target_array != nullptr && assign_array_words(*target_array, words, append);
}

bool VariableManager::assign_associative_literal(const std::string& name,
                                                 const std::vector<std::string>& words,
                                                 bool append) {
    return assign_associative_literal_impl(name, words, append, false);
}

bool VariableManager::assign_global_associative_literal(const std::string& name,
                                                        const std::vector<std::string>& words,
                                                        bool append) {
    return assign_associative_literal_impl(name, words, append, true);
}

bool VariableManager::assign_associative_literal_impl(const std::string& name,
                                                      const std::vector<std::string>& words,
                                                      bool append, bool force_global) {
    std::string resolved_name = resolve_nameref_reference(name);
    if (resolved_name != name) {
        return assign_associative_literal_impl(resolved_name, words, append, force_global);
    }
    if (!is_valid_identifier(name)) {
        return false;
    }

    const bool local_scope = !force_global && should_assign_to_local_scope(name);
    AssociativeArray* target_array = nullptr;
    if (local_scope) {
        if (local_associative_array_stack.empty()) {
            return false;
        }
        auto& arrays = local_associative_array_stack.back();
        auto [array_it, inserted] = arrays.emplace(name, AssociativeArray{});
        target_array = &array_it->second;
        auto& scalars = local_variable_stack.back();
        auto scalar_it = scalars.find(name);
        if (append && inserted && scalar_it != scalars.end()) {
            (*target_array)["0"] = scalar_it->second;
        }
        if (!append) {
            target_array->clear();
        }
        (void)scalars.erase(name);
        (void)local_array_stack.back().erase(name);
        (void)local_nameref_stack.back().erase(name);
    } else {
        auto [array_it, inserted] =
            global_associative_array_variables.emplace(name, AssociativeArray{});
        target_array = &array_it->second;
        if (append && inserted && has_global_scalar_binding(name)) {
            (*target_array)["0"] = get_global_scalar_value(name);
        }
        if (!append) {
            target_array->clear();
        }
        remove_global_scalar_binding(name);
        (void)global_array_variables.erase(name);
        (void)global_nameref_variables.erase(name);
    }
    return target_array != nullptr && assign_associative_words(*target_array, words, append);
}

bool VariableManager::assign_associative_words(AssociativeArray& target_array,
                                               const std::vector<std::string>& words, bool append) {
    size_t cursor = target_array.size();
    for (const std::string& word : words) {
        bool element_append = false;
        std::string key;
        std::string value;

        if (!word.empty() && word.front() == '[') {
            size_t close_bracket = word.find(']');
            if (close_bracket != std::string::npos && close_bracket > 1) {
                if (close_bracket + 1 < word.size() && word[close_bracket + 1] == '=') {
                    key = word.substr(1, close_bracket - 1);
                    value = word.substr(close_bracket + 2);
                } else if (close_bracket + 2 < word.size() && word[close_bracket + 1] == '+' &&
                           word[close_bracket + 2] == '=') {
                    element_append = true;
                    key = word.substr(1, close_bracket - 1);
                    value = word.substr(close_bracket + 3);
                }
            }
        }

        if (key.empty()) {
            key = std::to_string(cursor++);
            value = word;
        } else {
            key = normalize_associative_key(key);
        }
        if (key.empty()) {
            return false;
        }
        assign_string_value(target_array[key], value, element_append);
    }
    return true;
}

bool VariableManager::set_nameref(const std::string& name, const std::string& target,
                                  bool force_global) {
    if (!is_valid_identifier(name) || name == target) {
        return false;
    }
    std::string normalized_target = trim_whitespace(target);
    if (normalized_target.size() >= 2 &&
        ((normalized_target.front() == '\'' && normalized_target.back() == '\'') ||
         (normalized_target.front() == '"' && normalized_target.back() == '"'))) {
        normalized_target = normalized_target.substr(1, normalized_target.size() - 2);
    }
    ParsedArrayReference parsed;
    if (!normalized_target.empty() && !parse_array_reference(normalized_target, parsed)) {
        return false;
    }

    const bool local_scope = !force_global && !local_nameref_stack.empty();
    if (local_scope) {
        local_nameref_stack.back()[name] = normalized_target;
        (void)local_variable_stack.back().erase(name);
        (void)local_array_stack.back().erase(name);
        (void)local_associative_array_stack.back().erase(name);
    } else {
        global_nameref_variables[name] = normalized_target;
        remove_global_scalar_binding(name);
        (void)global_array_variables.erase(name);
        (void)global_associative_array_variables.erase(name);
    }
    return true;
}

bool VariableManager::is_nameref(const std::string& name) const {
    if (!local_nameref_stack.empty() &&
        local_nameref_stack.back().find(name) != local_nameref_stack.back().end()) {
        return true;
    }
    return global_nameref_variables.find(name) != global_nameref_variables.end();
}

std::string VariableManager::get_nameref_target(const std::string& name) const {
    if (!local_nameref_stack.empty()) {
        auto local_it = local_nameref_stack.back().find(name);
        if (local_it != local_nameref_stack.back().end()) {
            return local_it->second;
        }
    }
    auto global_it = global_nameref_variables.find(name);
    return global_it == global_nameref_variables.end() ? std::string{} : global_it->second;
}

bool VariableManager::unset_nameref(const std::string& name) {
    if (!local_nameref_stack.empty() && local_nameref_stack.back().erase(name) > 0) {
        return true;
    }
    return global_nameref_variables.erase(name) > 0;
}

bool VariableManager::is_indexed_array(const std::string& name) const {
    if (get_local_array(name) != nullptr || get_global_array(name) != nullptr) {
        return true;
    }
    std::string resolved = resolve_nameref_reference(name);
    return resolved != name &&
           (get_local_array(resolved) != nullptr || get_global_array(resolved) != nullptr);
}

bool VariableManager::is_associative_array(const std::string& name) const {
    if (get_local_associative_array(name) != nullptr ||
        get_global_associative_array(name) != nullptr) {
        return true;
    }
    std::string resolved = resolve_nameref_reference(name);
    return resolved != name && (get_local_associative_array(resolved) != nullptr ||
                                get_global_associative_array(resolved) != nullptr);
}

std::vector<std::pair<std::string, std::string>> VariableManager::get_array_entries(
    const std::string& name) const {
    std::string resolved = resolve_nameref_reference(name);
    const std::string& array_name = resolved.empty() ? name : resolved;

    std::vector<std::pair<std::string, std::string>> entries;
    if (const IndexedArray* indexed = get_local_array(array_name); indexed != nullptr) {
        entries.reserve(indexed->size());
        for (const auto& [index, value] : *indexed) {
            entries.emplace_back(std::to_string(index), value);
        }
        return entries;
    }
    if (const IndexedArray* indexed = get_global_array(array_name); indexed != nullptr) {
        entries.reserve(indexed->size());
        for (const auto& [index, value] : *indexed) {
            entries.emplace_back(std::to_string(index), value);
        }
        return entries;
    }
    if (const AssociativeArray* associative = get_local_associative_array(array_name);
        associative != nullptr) {
        entries.assign(associative->begin(), associative->end());
        return entries;
    }
    if (const AssociativeArray* associative = get_global_associative_array(array_name);
        associative != nullptr) {
        entries.assign(associative->begin(), associative->end());
    }
    return entries;
}

bool VariableManager::assign_array_words(IndexedArray& target_array,
                                         const std::vector<std::string>& words, bool append) {
    long long cursor = 0;
    if (append && !target_array.empty()) {
        cursor = target_array.rbegin()->first + 1;
    }

    for (const std::string& word : words) {
        bool explicit_element = false;
        bool element_append = false;
        std::string index_expr;
        std::string element_value;

        if (word.size() >= 4 && word.front() == '[') {
            size_t close_bracket = word.find(']');
            if (close_bracket != std::string::npos && close_bracket > 1) {
                if (close_bracket + 1 < word.size() && word[close_bracket + 1] == '=') {
                    explicit_element = true;
                    index_expr = word.substr(1, close_bracket - 1);
                    element_value = word.substr(close_bracket + 2);
                } else if (close_bracket + 2 < word.size() && word[close_bracket + 1] == '+' &&
                           word[close_bracket + 2] == '=') {
                    explicit_element = true;
                    element_append = true;
                    index_expr = word.substr(1, close_bracket - 1);
                    element_value = word.substr(close_bracket + 3);
                }
            }
        }

        if (explicit_element) {
            if (is_array_join_index(index_expr)) {
                return false;
            }

            auto index = evaluate_array_index_expression(index_expr);
            if (!index.has_value()) {
                return false;
            }

            if (element_append) {
                target_array[*index] += element_value;
            } else {
                target_array[*index] = element_value;
            }
            cursor = *index + 1;
            continue;
        }

        target_array[cursor] = word;
        cursor++;
    }

    return true;
}

bool VariableManager::is_local_variable(const std::string& name) const {
    if (local_variable_stack.empty()) {
        return false;
    }

    const auto& current_scope = local_variable_stack.back();
    if (current_scope.find(name) != current_scope.end()) {
        return true;
    }

    return has_local_array_binding(name) || has_local_associative_array_binding(name) ||
           has_local_nameref_binding(name);
}

bool VariableManager::unset_local_variable(const std::string& name) {
    if (local_variable_stack.empty()) {
        return false;
    }

    bool removed = false;
    auto& current_scope = local_variable_stack.back();
    auto scalar_it = current_scope.find(name);
    if (scalar_it != current_scope.end()) {
        (void)current_scope.erase(scalar_it);
        removed = true;
    }

    if (!local_array_stack.empty()) {
        auto& current_arrays = local_array_stack.back();
        auto array_it = current_arrays.find(name);
        if (array_it != current_arrays.end()) {
            (void)current_arrays.erase(array_it);
            removed = true;
        }
    }

    if (!local_associative_array_stack.empty()) {
        removed = local_associative_array_stack.back().erase(name) > 0 || removed;
    }
    if (!local_nameref_stack.empty()) {
        removed = local_nameref_stack.back().erase(name) > 0 || removed;
    }

    return removed;
}

bool VariableManager::unset_variable(const std::string& target) {
    std::string resolved_target = resolve_nameref_reference(target);
    if (resolved_target != target) {
        return unset_variable(resolved_target);
    }
    ParsedArrayReference parsed;
    if (!parse_array_reference(target, parsed)) {
        return false;
    }

    bool local_scope = should_assign_to_local_scope(parsed.name);

    if (parsed.has_index) {
        if (is_array_join_index(parsed.index)) {
            if (local_scope) {
                bool removed = unset_local_variable(parsed.name);
                return removed;
            }

            bool removed = false;
            if (global_array_variables.erase(parsed.name) > 0) {
                removed = true;
            }
            if (global_associative_array_variables.erase(parsed.name) > 0) {
                removed = true;
            }
            if (has_global_scalar_binding(parsed.name)) {
                remove_global_scalar_binding(parsed.name);
                removed = true;
            }
            return removed;
        }

        auto index = evaluate_array_index_expression(parsed.index);
        if (local_scope) {
            AssociativeArray* associative = get_local_associative_array(parsed.name);
            if (associative != nullptr) {
                return associative->erase(normalize_associative_key(parsed.index)) > 0;
            }
            if (!index.has_value()) {
                return false;
            }
            IndexedArray* array = get_local_array(parsed.name);
            if (array == nullptr) {
                return false;
            }
            return array->erase(*index) > 0;
        }

        AssociativeArray* associative = get_global_associative_array(parsed.name);
        if (associative != nullptr) {
            return associative->erase(normalize_associative_key(parsed.index)) > 0;
        }
        if (!index.has_value()) {
            return false;
        }
        IndexedArray* array = get_global_array(parsed.name);
        if (array == nullptr) {
            return false;
        }
        return array->erase(*index) > 0;
    }

    if (local_scope) {
        return unset_local_variable(parsed.name);
    }

    bool removed = false;
    if (global_array_variables.erase(parsed.name) > 0) {
        removed = true;
    }
    if (global_associative_array_variables.erase(parsed.name) > 0) {
        removed = true;
    }
    if (global_nameref_variables.erase(parsed.name) > 0) {
        removed = true;
    }
    if (has_global_scalar_binding(parsed.name)) {
        remove_global_scalar_binding(parsed.name);
        removed = true;
    }
    return removed;
}

void VariableManager::mark_local_as_exported(const std::string& name) {
    if (!exported_locals_stack.empty() && !saved_env_stack.empty()) {
        // Raw getenv here: variable manager mirrors process env.
        const char* old_val = getenv(name.c_str());
        std::string old_value = (old_val != nullptr) ? old_val : "";

        (void)saved_env_stack.back().emplace_back(name, old_value);
        exported_locals_stack.back().push_back(name);
    }
}

bool VariableManager::in_function_scope() const {
    return !local_variable_stack.empty();
}

VariableManager::VariableLookup VariableManager::lookup_variable(const std::string& name,
                                                                 bool include_value) const {
    const std::string var_name = resolve_nameref_reference(name);
    ParsedArrayReference parsed;
    const bool indexed = var_name.find('[') != std::string::npos &&
                         parse_array_reference(var_name, parsed) && parsed.has_index;
    const std::string& key = indexed ? parsed.name : var_name;
    const bool join = indexed && is_array_join_index(parsed.index);

    auto value_result = [include_value](const std::string* value) -> VariableLookup {
        return {include_value && value != nullptr ? *value : std::string{}, value != nullptr};
    };
    auto indexed_value = [&](const IndexedArray& array) -> VariableLookup {
        if (join) {
            return {include_value ? join_array_values(array) : std::string{}, !array.empty()};
        }
        const auto index =
            indexed ? evaluate_array_index_expression(parsed.index) : std::optional<long long>{0};
        if (!index) {
            return {};
        }
        const auto it = array.find(*index);
        return value_result(it == array.end() ? nullptr : &it->second);
    };
    auto associative_value = [&](const AssociativeArray& array) -> VariableLookup {
        if (join) {
            return {include_value ? join_associative_values(array) : std::string{}, !array.empty()};
        }
        const auto it = array.find(indexed ? normalize_associative_key(parsed.index) : "0");
        return value_result(it == array.end() ? nullptr : &it->second);
    };
    auto scalar_element = [&](VariableLookup result) -> VariableLookup {
        if (!result.is_set || join) {
            return result;
        }
        const auto index = evaluate_array_index_expression(parsed.index);
        return index && *index == 0 ? result : VariableLookup{};
    };

    if (indexed) {
        // Explicit subscripts use the same associative/indexed/scalar precedence in
        // either scope. Presence queries avoid materializing values or array joins.
        auto scope_value = [&](bool local) -> std::optional<VariableLookup> {
            const auto* associative =
                local ? get_local_associative_array(key) : get_global_associative_array(key);
            if (associative != nullptr) {
                return associative_value(*associative);
            }
            const auto* array = local ? get_local_array(key) : get_global_array(key);
            if (array != nullptr) {
                return indexed_value(*array);
            }
            if (local) {
                const auto& scalars = local_variable_stack.back();
                const auto it = scalars.find(key);
                if (it != scalars.end()) {
                    return scalar_element(value_result(&it->second));
                }
                return std::nullopt;
            }
            return scalar_element(lookup_global_scalar(key, include_value));
        };
        if (!local_variable_stack.empty()) {
            if (auto local = scope_value(true)) {
                return std::move(*local);
            }
        }
        return scope_value(false).value();
    }

    if (!local_variable_stack.empty()) {
        const auto& scalars = local_variable_stack.back();
        const auto it = scalars.find(key);
        if (it != scalars.end()) {
            return value_result(&it->second);
        }
        if (const auto* array = get_local_array(key)) {
            auto result = indexed_value(*array);
            if (result.is_set) {
                return result;
            }
        }
        if (const auto* array = get_local_associative_array(key)) {
            auto result = associative_value(*array);
            if (include_value || result.is_set) {
                return result;
            }
        }
    }

    if (parameter_utils::is_named_special_parameter_name(key)) {
        return {include_value ? get_special_variable(key) : std::string{}, true};
    }

    if (!key.empty() && std::all_of(key.begin(), key.end(),
                                    [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        VariableLookup positional;
        // Raw getenv here: positional parameters can be mirrored in the process environment.
        if (const char* value = getenv(key.c_str())) {
            positional = {include_value ? value : "", true};
        } else {
            int number = 0;
            if (numeric_utils::parse_int_strict(key, number) && number > 0 &&
                static_cast<size_t>(number - 1) < flags::get_positional_parameter_count()) {
                positional.is_set = true;
                if (include_value) {
                    positional.value =
                        flags::get_positional_parameters()[static_cast<size_t>(number - 1)];
                }
            }
        }
        if (!include_value || !positional.value.empty() || key.size() == 1) {
            return positional;
        }
    }

    // A value read of a global array without element zero stays empty; the existing
    // presence check can still fall through to an environment binding of the same name.
    if (const auto* array = get_global_array(key)) {
        auto result = indexed_value(*array);
        if (include_value || result.is_set) {
            return result;
        }
    }
    if (const auto* array = get_global_associative_array(key)) {
        auto result = associative_value(*array);
        if (include_value || result.is_set) {
            return result;
        }
    }
    return lookup_global_scalar(key, include_value);
}

std::string VariableManager::get_variable_value(const std::string& var_name) const {
    return lookup_variable(var_name, true).value;
}

std::string VariableManager::get_indirect_value(const std::string& var_name) const {
    if (is_nameref(var_name)) {
        return get_nameref_target(var_name);
    }
    return get_variable_value(get_variable_value(var_name));
}

bool VariableManager::variable_is_set(const std::string& var_name) const {
    return lookup_variable(var_name, false).is_set;
}

std::optional<size_t> VariableManager::get_array_length(const std::string& var_name) const {
    std::string resolved_name = resolve_nameref_reference(var_name);
    if (resolved_name != var_name) {
        return get_array_length(resolved_name);
    }
    ParsedArrayReference parsed;
    if (!parse_array_reference(var_name, parsed) || !parsed.has_index ||
        !is_array_join_index(parsed.index)) {
        return std::nullopt;
    }

    const IndexedArray* local_array = get_local_array(parsed.name);
    if (local_array != nullptr) {
        return local_array->size();
    }
    const AssociativeArray* local_associative = get_local_associative_array(parsed.name);
    if (local_associative != nullptr) {
        return local_associative->size();
    }
    if (!local_variable_stack.empty() &&
        local_variable_stack.back().find(parsed.name) != local_variable_stack.back().end()) {
        return 1;
    }

    const IndexedArray* global_array = get_global_array(parsed.name);
    if (global_array != nullptr) {
        return global_array->size();
    }
    const AssociativeArray* global_associative = get_global_associative_array(parsed.name);
    if (global_associative != nullptr) {
        return global_associative->size();
    }
    if (has_global_scalar_binding(parsed.name)) {
        return 1;
    }

    return std::nullopt;
}

std::string VariableManager::get_array_keys(const std::string& var_name) const {
    std::string resolved_name = resolve_nameref_reference(var_name);
    if (resolved_name != var_name) {
        return get_array_keys(resolved_name);
    }
    ParsedArrayReference parsed;
    if (!parse_array_reference(var_name, parsed) || !parsed.has_index ||
        !is_array_join_index(parsed.index)) {
        return "";
    }

    const IndexedArray* local_array = get_local_array(parsed.name);
    if (local_array != nullptr) {
        return join_array_keys(*local_array);
    }
    const AssociativeArray* local_associative = get_local_associative_array(parsed.name);
    if (local_associative != nullptr) {
        return join_associative_keys(*local_associative);
    }
    if (!local_variable_stack.empty() &&
        local_variable_stack.back().find(parsed.name) != local_variable_stack.back().end()) {
        return "0";
    }

    const IndexedArray* global_array = get_global_array(parsed.name);
    if (global_array != nullptr) {
        return join_array_keys(*global_array);
    }
    const AssociativeArray* global_associative = get_global_associative_array(parsed.name);
    if (global_associative != nullptr) {
        return join_associative_keys(*global_associative);
    }
    if (has_global_scalar_binding(parsed.name)) {
        return "0";
    }

    return "";
}

std::vector<std::string> VariableManager::get_variable_names() const {
    std::unordered_set<std::string> name_set;

    for (const auto& scope : local_variable_stack) {
        for (const auto& entry : scope) {
            (void)name_set.insert(entry.first);
        }
    }
    for (const auto& scope : local_array_stack) {
        for (const auto& entry : scope) {
            (void)name_set.insert(entry.first);
        }
    }
    for (const auto& scope : local_associative_array_stack) {
        for (const auto& entry : scope) {
            (void)name_set.insert(entry.first);
        }
    }
    for (const auto& scope : local_nameref_stack) {
        for (const auto& entry : scope) {
            (void)name_set.insert(entry.first);
        }
    }

    for (const auto& entry : global_array_variables) {
        (void)name_set.insert(entry.first);
    }
    for (const auto& entry : global_associative_array_variables) {
        (void)name_set.insert(entry.first);
    }
    for (const auto& entry : global_nameref_variables) {
        (void)name_set.insert(entry.first);
    }

    if (g_shell) {
        const auto& env_vars = cjsh_env::env_vars();
        for (const auto& entry : env_vars) {
            (void)name_set.insert(entry.first);
        }
    }

    std::vector<std::string> names;
    names.reserve(name_set.size());
    for (const auto& name : name_set) {
        names.push_back(name);
    }
    return names;
}

bool VariableManager::parse_array_reference(const std::string& target,
                                            ParsedArrayReference& parsed) const {
    std::string trimmed = trim_whitespace(target);
    if (trimmed.empty()) {
        return false;
    }

    size_t left_bracket = trimmed.find('[');
    if (left_bracket == std::string::npos) {
        if (!is_valid_identifier(trimmed)) {
            return false;
        }
        parsed.name = trimmed;
        parsed.index.clear();
        parsed.has_index = false;
        return true;
    }

    if (trimmed.back() != ']') {
        return false;
    }

    std::string name = trimmed.substr(0, left_bracket);
    if (!is_valid_identifier(name)) {
        return false;
    }

    std::string index = trimmed.substr(left_bracket + 1, trimmed.length() - left_bracket - 2);
    if (index.empty()) {
        return false;
    }

    parsed.name = std::move(name);
    parsed.index = std::move(index);
    parsed.has_index = true;
    return true;
}

std::optional<long long> VariableManager::evaluate_array_index_expression(
    const std::string& expr) const {
    std::string trimmed = trim_whitespace(expr);
    if (trimmed.empty() || is_array_join_index(trimmed)) {
        return std::nullopt;
    }

    auto var_reader = [this](const std::string& name) -> long long {
        std::string value = get_variable_value(name);
        if (value.empty()) {
            return 0;
        }
        try {
            return std::stoll(value);
        } catch (const std::exception&) {
            return 0;
        }
    };
    auto var_writer = [](const std::string&, long long) {};

    ArithmeticEvaluator evaluator(var_reader, var_writer);
    try {
        long long result = evaluator.evaluate(trimmed);
        if (result < 0) {
            return std::nullopt;
        }
        return result;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string VariableManager::join_array_values(const IndexedArray& array) const {
    if (array.empty()) {
        return "";
    }

    std::string joined;
    bool first = true;
    for (const auto& [_, value] : array) {
        if (!first) {
            joined.push_back(' ');
        }
        joined += value;
        first = false;
    }
    return joined;
}

std::string VariableManager::join_array_keys(const IndexedArray& array) const {
    if (array.empty()) {
        return "";
    }

    std::string joined;
    bool first = true;
    for (const auto& [index, _] : array) {
        if (!first) {
            joined.push_back(' ');
        }
        joined += std::to_string(index);
        first = false;
    }
    return joined;
}

std::string VariableManager::join_associative_values(const AssociativeArray& array) const {
    std::string joined;
    for (const auto& [_, value] : array) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += value;
    }
    return joined;
}

std::string VariableManager::join_associative_keys(const AssociativeArray& array) const {
    std::string joined;
    for (const auto& [key, _] : array) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += key;
    }
    return joined;
}

bool VariableManager::has_local_array_binding(const std::string& name) const {
    if (local_array_stack.empty()) {
        return false;
    }
    return local_array_stack.back().find(name) != local_array_stack.back().end();
}

bool VariableManager::has_local_associative_array_binding(const std::string& name) const {
    return !local_associative_array_stack.empty() &&
           local_associative_array_stack.back().find(name) !=
               local_associative_array_stack.back().end();
}

bool VariableManager::has_local_nameref_binding(const std::string& name) const {
    return !local_nameref_stack.empty() &&
           local_nameref_stack.back().find(name) != local_nameref_stack.back().end();
}

bool VariableManager::has_local_binding(const std::string& name) const {
    if (local_variable_stack.empty()) {
        return false;
    }
    if (local_variable_stack.back().find(name) != local_variable_stack.back().end()) {
        return true;
    }
    return has_local_array_binding(name) || has_local_associative_array_binding(name) ||
           has_local_nameref_binding(name);
}

bool VariableManager::should_assign_to_local_scope(const std::string& name) const {
    if (local_variable_stack.empty()) {
        return false;
    }
    return has_local_binding(name);
}

VariableManager::IndexedArray* VariableManager::get_local_array(const std::string& name) {
    if (local_array_stack.empty()) {
        return nullptr;
    }
    auto it = local_array_stack.back().find(name);
    if (it == local_array_stack.back().end()) {
        return nullptr;
    }
    return &it->second;
}

const VariableManager::IndexedArray* VariableManager::get_local_array(
    const std::string& name) const {
    if (local_array_stack.empty()) {
        return nullptr;
    }
    auto it = local_array_stack.back().find(name);
    if (it == local_array_stack.back().end()) {
        return nullptr;
    }
    return &it->second;
}

VariableManager::IndexedArray* VariableManager::get_global_array(const std::string& name) {
    auto it = global_array_variables.find(name);
    if (it == global_array_variables.end()) {
        return nullptr;
    }
    return &it->second;
}

const VariableManager::IndexedArray* VariableManager::get_global_array(
    const std::string& name) const {
    auto it = global_array_variables.find(name);
    if (it == global_array_variables.end()) {
        return nullptr;
    }
    return &it->second;
}

VariableManager::AssociativeArray* VariableManager::get_local_associative_array(
    const std::string& name) {
    if (local_associative_array_stack.empty()) {
        return nullptr;
    }
    auto it = local_associative_array_stack.back().find(name);
    return it == local_associative_array_stack.back().end() ? nullptr : &it->second;
}

const VariableManager::AssociativeArray* VariableManager::get_local_associative_array(
    const std::string& name) const {
    if (local_associative_array_stack.empty()) {
        return nullptr;
    }
    auto it = local_associative_array_stack.back().find(name);
    return it == local_associative_array_stack.back().end() ? nullptr : &it->second;
}

VariableManager::AssociativeArray* VariableManager::get_global_associative_array(
    const std::string& name) {
    auto it = global_associative_array_variables.find(name);
    return it == global_associative_array_variables.end() ? nullptr : &it->second;
}

const VariableManager::AssociativeArray* VariableManager::get_global_associative_array(
    const std::string& name) const {
    auto it = global_associative_array_variables.find(name);
    return it == global_associative_array_variables.end() ? nullptr : &it->second;
}

bool VariableManager::assign_scalar_value(const std::string& name, const std::string& value,
                                          bool append, bool local_scope) {
    if (local_scope) {
        if (local_variable_stack.empty()) {
            return false;
        }

        AssociativeArray* local_associative = get_local_associative_array(name);
        if (local_associative != nullptr) {
            assign_string_value((*local_associative)["0"], value, append);
            return true;
        }

        IndexedArray* local_array = get_local_array(name);
        if (local_array != nullptr) {
            assign_string_value((*local_array)[0], value, append);
            return true;
        }

        auto& scalars = local_variable_stack.back();
        assign_string_value(scalars[name], value, append);
        return true;
    }

    AssociativeArray* global_associative = get_global_associative_array(name);
    if (global_associative != nullptr) {
        assign_string_value((*global_associative)["0"], value, append);
        return true;
    }

    IndexedArray* global_array = get_global_array(name);
    if (global_array != nullptr) {
        assign_string_value((*global_array)[0], value, append);
        return true;
    }

    if (append && has_global_scalar_binding(name)) {
        std::string current_value = get_global_scalar_value(name);
        set_environment_variable(name, current_value + value);
        return true;
    }

    set_environment_variable(name, value);
    return true;
}

bool VariableManager::assign_array_element_value(const std::string& name,
                                                 const std::string& index_expr,
                                                 const std::string& value, bool append,
                                                 bool local_scope) {
    if (is_array_join_index(index_expr)) {
        return false;
    }

    if (local_scope) {
        if (local_array_stack.empty()) {
            return false;
        }

        AssociativeArray* associative = get_local_associative_array(name);
        if (associative != nullptr) {
            std::string key = normalize_associative_key(index_expr);
            if (key.empty()) {
                return false;
            }
            assign_string_value((*associative)[key], value, append);
            return true;
        }

        auto index = evaluate_array_index_expression(index_expr);
        if (!index.has_value()) {
            return false;
        }

        auto& arrays = local_array_stack.back();
        auto& scalars = local_variable_stack.back();
        auto scalar_it = scalars.find(name);

        auto [array_it, inserted] = arrays.emplace(name, IndexedArray{});
        IndexedArray& target = array_it->second;
        if (inserted && scalar_it != scalars.end()) {
            (void)target.emplace(0, scalar_it->second);
            (void)scalars.erase(scalar_it);
        }

        assign_string_value(target[*index], value, append);
        return true;
    }

    AssociativeArray* associative = get_global_associative_array(name);
    if (associative != nullptr) {
        std::string key = normalize_associative_key(index_expr);
        if (key.empty()) {
            return false;
        }
        assign_string_value((*associative)[key], value, append);
        return true;
    }

    auto index = evaluate_array_index_expression(index_expr);
    if (!index.has_value()) {
        return false;
    }

    auto [array_it, inserted] = global_array_variables.emplace(name, IndexedArray{});
    IndexedArray& target = array_it->second;

    if (inserted && has_global_scalar_binding(name)) {
        (void)target.emplace(0, get_global_scalar_value(name));
        remove_global_scalar_binding(name);
    }

    assign_string_value(target[*index], value, append);

    return true;
}

VariableManager::VariableLookup VariableManager::lookup_global_scalar(const std::string& name,
                                                                      bool include_value) const {
    if (g_shell) {
        const auto& variables = cjsh_env::env_vars();
        const auto it = variables.find(name);
        if (it != variables.end()) {
            return {include_value ? it->second : std::string{}, true};
        }
    }
    // Raw getenv here: variable manager mirrors process env.
    const char* value = getenv(name.c_str());
    return {include_value && value != nullptr ? value : "", value != nullptr};
}

bool VariableManager::has_global_scalar_binding(const std::string& name) const {
    return lookup_global_scalar(name, false).is_set;
}

std::string VariableManager::get_global_scalar_value(const std::string& name) const {
    return lookup_global_scalar(name, true).value;
}

void VariableManager::remove_global_scalar_binding(const std::string& name) {
    if (g_shell) {
        auto& env_vars = cjsh_env::env_vars();
        (void)env_vars.erase(name);

        if (auto* parser = g_shell->get_parser()) {
            parser->unset_env_var(name);
        }
    }

    (void)unsetenv(name.c_str());
    cjsh_env::mirror_unset_from_process_env(name);
}

std::string VariableManager::normalize_associative_key(const std::string& key) const {
    std::string normalized = trim_whitespace(key);
    if (normalized.size() >= 2 && ((normalized.front() == '\'' && normalized.back() == '\'') ||
                                   (normalized.front() == '"' && normalized.back() == '"'))) {
        normalized = normalized.substr(1, normalized.size() - 2);
    }
    if (normalized.size() >= 3 && normalized.front() == '$' && normalized[1] == '{' &&
        normalized.back() == '}') {
        normalized = get_variable_value(normalized.substr(2, normalized.size() - 3));
    } else if (normalized.size() >= 2 && normalized.front() == '$' &&
               is_valid_identifier(normalized.substr(1))) {
        normalized = get_variable_value(normalized.substr(1));
    }
    return normalized;
}

std::string VariableManager::resolve_nameref_reference(const std::string& reference) const {
    std::string current = trim_whitespace(reference);
    // Only the current local scope and the global table participate in nameref
    // resolution. Ordinary scalar reads need no reference parsing when both are empty.
    if (global_nameref_variables.empty() &&
        (local_nameref_stack.empty() || local_nameref_stack.back().empty())) {
        return current;
    }
    std::unordered_set<std::string> visited;

    for (size_t depth = 0; depth < 64; ++depth) {
        ParsedArrayReference parsed;
        if (!parse_array_reference(current, parsed) || !is_nameref(parsed.name) ||
            !visited.insert(parsed.name).second) {
            break;
        }

        std::string target = get_nameref_target(parsed.name);
        if (target.empty()) {
            break;
        }
        if (parsed.has_index && target.find('[') == std::string::npos) {
            target += '[' + parsed.index + ']';
        }
        current = std::move(target);
    }
    return current;
}

std::string VariableManager::get_special_variable(const std::string& var_name) const {
    return parameter_utils::get_special_parameter_value(var_name);
}
