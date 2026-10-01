/*
  variable_manager.h

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

#ifndef CJSH_CORE_SRC_INTERPRETER_VARIABLE_MANAGER_H
#define CJSH_CORE_SRC_INTERPRETER_VARIABLE_MANAGER_H

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class VariableManager {
   public:
    using VariableMap = std::unordered_map<std::string, std::string>;
    using VariableStack = std::vector<VariableMap>;
    using IndexedArray = std::map<long long, std::string>;
    using ArrayMap = std::unordered_map<std::string, IndexedArray>;
    using ArrayStack = std::vector<ArrayMap>;
    using AssociativeArray = std::map<std::string, std::string>;
    using AssociativeArrayMap = std::unordered_map<std::string, AssociativeArray>;
    using AssociativeArrayStack = std::vector<AssociativeArrayMap>;
    using NamerefMap = std::unordered_map<std::string, std::string>;
    using NamerefStack = std::vector<NamerefMap>;
    using ExportedLocalsStack = std::vector<std::vector<std::string>>;

    VariableManager() = default;

    VariableManager(const VariableManager&) = delete;
    VariableManager& operator=(const VariableManager&) = delete;
    VariableManager(VariableManager&&) = default;
    VariableManager& operator=(VariableManager&&) = default;

    void push_scope();
    void pop_scope();

    void set_local_variable(const std::string& name, const std::string& value);
    void set_environment_variable(const std::string& name, const std::string& value);
    bool assign_variable(const std::string& target, const std::string& value, bool append = false);
    bool assign_global_variable(const std::string& target, const std::string& value,
                                bool append = false);
    bool assign_array_literal(const std::string& name, const std::vector<std::string>& words,
                              bool append = false);
    bool assign_global_array_literal(const std::string& name, const std::vector<std::string>& words,
                                     bool append = false);
    bool assign_associative_literal(const std::string& name, const std::vector<std::string>& words,
                                    bool append = false);
    bool assign_global_associative_literal(const std::string& name,
                                           const std::vector<std::string>& words,
                                           bool append = false);
    bool set_nameref(const std::string& name, const std::string& target, bool force_global = false);
    bool is_nameref(const std::string& name) const;
    std::string get_nameref_target(const std::string& name) const;
    bool unset_nameref(const std::string& name);
    bool is_indexed_array(const std::string& name) const;
    bool is_associative_array(const std::string& name) const;
    std::vector<std::pair<std::string, std::string>> get_array_entries(
        const std::string& name) const;
    bool is_local_variable(const std::string& name) const;
    bool unset_local_variable(const std::string& name);
    bool unset_variable(const std::string& target);
    void mark_local_as_exported(const std::string& name);
    bool in_function_scope() const;

    std::string get_variable_value(const std::string& var_name) const;
    std::string get_indirect_value(const std::string& var_name) const;
    bool variable_is_set(const std::string& var_name) const;
    std::optional<size_t> get_array_length(const std::string& var_name) const;
    std::string get_array_keys(const std::string& var_name) const;
    std::vector<std::string> get_variable_names() const;

   private:
    struct VariableLookup {
        std::string value;
        bool is_set = false;
    };
    VariableLookup lookup_variable(const std::string& name, bool include_value) const;
    VariableLookup lookup_global_scalar(const std::string& name, bool include_value) const;

    bool assign_array_literal_impl(const std::string& name, const std::vector<std::string>& words,
                                   bool append, bool force_global);
    bool assign_associative_literal_impl(const std::string& name,
                                         const std::vector<std::string>& words, bool append,
                                         bool force_global);
    struct ParsedArrayReference {
        std::string name;
        std::string index;
        bool has_index = false;
    };

    VariableStack local_variable_stack;
    ArrayStack local_array_stack;
    ArrayMap global_array_variables;
    AssociativeArrayStack local_associative_array_stack;
    AssociativeArrayMap global_associative_array_variables;
    NamerefStack local_nameref_stack;
    NamerefMap global_nameref_variables;
    ExportedLocalsStack exported_locals_stack;

    std::vector<std::vector<std::pair<std::string, std::string>>> saved_env_stack;

    bool parse_array_reference(const std::string& target, ParsedArrayReference& parsed) const;
    std::optional<long long> evaluate_array_index_expression(const std::string& expr) const;

    std::string join_array_values(const IndexedArray& array) const;
    std::string join_array_keys(const IndexedArray& array) const;
    std::string join_associative_values(const AssociativeArray& array) const;
    std::string join_associative_keys(const AssociativeArray& array) const;

    bool has_local_array_binding(const std::string& name) const;
    bool has_local_associative_array_binding(const std::string& name) const;
    bool has_local_nameref_binding(const std::string& name) const;
    bool has_local_binding(const std::string& name) const;
    bool should_assign_to_local_scope(const std::string& name) const;

    IndexedArray* get_local_array(const std::string& name);
    const IndexedArray* get_local_array(const std::string& name) const;
    IndexedArray* get_global_array(const std::string& name);
    const IndexedArray* get_global_array(const std::string& name) const;
    AssociativeArray* get_local_associative_array(const std::string& name);
    const AssociativeArray* get_local_associative_array(const std::string& name) const;
    AssociativeArray* get_global_associative_array(const std::string& name);
    const AssociativeArray* get_global_associative_array(const std::string& name) const;

    bool assign_scalar_value(const std::string& name, const std::string& value, bool append,
                             bool local_scope);
    bool assign_array_element_value(const std::string& name, const std::string& index_expr,
                                    const std::string& value, bool append, bool local_scope);
    bool assign_array_words(IndexedArray& target_array, const std::vector<std::string>& words,
                            bool append);
    bool assign_associative_words(AssociativeArray& target_array,
                                  const std::vector<std::string>& words, bool append);
    std::string normalize_associative_key(const std::string& key) const;
    std::string resolve_nameref_reference(const std::string& reference) const;
    bool has_global_scalar_binding(const std::string& name) const;
    std::string get_global_scalar_value(const std::string& name) const;
    void remove_global_scalar_binding(const std::string& name);

    std::string get_special_variable(const std::string& var_name) const;
};

#endif  // CJSH_CORE_SRC_INTERPRETER_VARIABLE_MANAGER_H
